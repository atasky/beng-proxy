/*
 * Copyright 2007-2022 CM4all GmbH
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "sink_fd.hxx"
#include "Sink.hxx"
#include "UnusedPtr.hxx"
#include "pool/pool.hxx"
#include "io/Splice.hxx"
#include "io/SpliceSupport.hxx"
#include "io/FileDescriptor.hxx"
#include "event/SocketEvent.hxx"
#include "util/DestructObserver.hxx"
#include "util/LeakDetector.hxx"

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>

class SinkFd final : IstreamSink, DestructAnchor, LeakDetector {
	FileDescriptor fd;
	const FdType fd_type;
	SinkFdHandler &handler;

	SocketEvent event;

	/**
	 * Set to true each time data was received from the istream.
	 */
	bool got_data;

	/**
	 * This flag is used to determine if the SocketEvent::WRITE event
	 * shall be scheduled after a splice().  We need to add the event
	 * only if the splice() was triggered by SocketEvent::WRITE,
	 * because then we're responsible for querying more data.
	 */
	bool got_event = false;

#ifndef NDEBUG
	bool valid = true;
#endif

public:
	SinkFd(EventLoop &event_loop,
	       UnusedIstreamPtr &&_istream,
	       FileDescriptor _fd, FdType _fd_type,
	       SinkFdHandler &_handler) noexcept
		:IstreamSink(std::move(_istream)),
		 fd(_fd), fd_type(_fd_type),
		 handler(_handler),
		 event(event_loop, BIND_THIS_METHOD(EventCallback),
		       SocketDescriptor::FromFileDescriptor(fd))
	{
		input.SetDirect(istream_direct_mask_to(fd_type));

		ScheduleWrite();
	}

	void Destroy() noexcept {
		this->~SinkFd();
	}

	bool IsDefined() const noexcept {
		return input.IsDefined();
	}

	void Read() noexcept {
		assert(valid);
		assert(IsDefined());

		input.Read();
	}

	void Close() noexcept {
#ifndef NDEBUG
		valid = false;
#endif

		Destroy();
	}

private:
	void ScheduleWrite() noexcept {
		assert(fd.IsDefined());
		assert(input.IsDefined());

		got_event = false;
		event.ScheduleWrite();
	}

	void EventCallback(unsigned events) noexcept;

	/* virtual methods from class IstreamHandler */
	std::size_t OnData(std::span<const std::byte> src) noexcept override;
	IstreamDirectResult OnDirect(FdType type, FileDescriptor fd,
				     off_t offset,
				     std::size_t max_length) noexcept override;
	void OnEof() noexcept override;
	void OnError(std::exception_ptr ep) noexcept override;
};

/*
 * istream handler
 *
 */

std::size_t
SinkFd::OnData(std::span<const std::byte> src) noexcept
{
	got_data = true;

	ssize_t nbytes = IsAnySocket(fd_type)
		? send(fd.Get(), src.data(), src.size(),
		       MSG_DONTWAIT|MSG_NOSIGNAL)
		: fd.Write(src.data(), src.size());
	if (nbytes >= 0) {
		ScheduleWrite();
		return nbytes;
	} else if (errno == EAGAIN) {
		ScheduleWrite();
		return 0;
	} else {
		event.Cancel();
		if (handler.OnSendError(errno)) {
			Destroy();
		}
		return 0;
	}
}

IstreamDirectResult
SinkFd::OnDirect(FdType type, FileDescriptor _fd, off_t offset,
		 std::size_t max_length) noexcept
{
	got_data = true;

	ssize_t nbytes = SpliceTo(_fd, type, ToOffsetPointer(offset),
				  fd, fd_type,
				  max_length);

	if (nbytes <= 0) {
		if (nbytes == 0)
			return IstreamDirectResult::END;

		if (errno != EAGAIN)
			return IstreamDirectResult::ERRNO;

		if (!fd.IsReadyForWriting()) {
			ScheduleWrite();
			return IstreamDirectResult::BLOCKING;
		}

		/* try again, just in case connection->fd has become
		   ready between the first istream_direct_to_socket()
		   call and fd_ready_for_writing() */
		nbytes = SpliceTo(_fd, type, ToOffsetPointer(offset),
				  fd, fd_type,
				  max_length);

		if (nbytes <= 0)
			return nbytes < 0
				? IstreamDirectResult::ERRNO
				: IstreamDirectResult::END;
	}

	input.ConsumeDirect(nbytes);

	if (got_event || type == FdType::FD_FILE)
		/* regular files don't have support for SocketEvent::READ, and
		   thus the sink is responsible for triggering the next
		   splice */
		ScheduleWrite();

	return IstreamDirectResult::OK;
}

void
SinkFd::OnEof() noexcept
{
	ClearInput();

	got_data = true;

#ifndef NDEBUG
	valid = false;
#endif

	event.Cancel();

	handler.OnInputEof();
	Destroy();
}

void
SinkFd::OnError(std::exception_ptr ep) noexcept
{
	ClearInput();

	got_data = true;

#ifndef NDEBUG
	valid = false;
#endif

	event.Cancel();

	handler.OnInputError(ep);
	Destroy();
}

/*
 * libevent callback
 *
 */

inline void
SinkFd::EventCallback(unsigned) noexcept
{
	const DestructObserver destructed(*this);

	got_event = true;
	got_data = false;
	input.Read();

	if (!destructed && !got_data)
		/* the fd is ready for writing, but the istream is blocking -
		   don't try again for now */
		event.Cancel();
}

/*
 * constructor
 *
 */

SinkFd *
sink_fd_new(EventLoop &event_loop, struct pool &pool, UnusedIstreamPtr istream,
	    FileDescriptor fd, FdType fd_type,
	    SinkFdHandler &handler) noexcept
{
	assert(fd.IsDefined());

	return NewFromPool<SinkFd>(pool, event_loop, std::move(istream),
				   fd, fd_type,
				   handler);
}

void
sink_fd_read(SinkFd *ss) noexcept
{
	assert(ss != nullptr);

	ss->Read();
}

void
sink_fd_close(SinkFd *ss) noexcept
{
	assert(ss != nullptr);

	ss->Close();
}
