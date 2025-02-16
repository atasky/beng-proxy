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

#include "StdioSink.hxx"
#include "FailingResourceLoader.hxx"
#include "PInstance.hxx"
#include "memory/fb_pool.hxx"
#include "bp/XmlProcessor.hxx"
#include "widget/Context.hxx"
#include "widget/Inline.hxx"
#include "widget/Widget.hxx"
#include "widget/Ptr.hxx"
#include "widget/RewriteUri.hxx"
#include "istream/OpenFileIstream.hxx"
#include "istream/istream_string.hxx"
#include "pool/SharedPtr.hxx"
#include "util/StringView.hxx"
#include "util/PrintException.hxx"
#include "stopwatch.hxx"

using std::string_view_literals::operator""sv;

/*
 * emulate missing libraries
 *
 */

UnusedIstreamPtr
embed_inline_widget(struct pool &pool,
		    SharedPoolPtr<WidgetContext>,
		    const StopwatchPtr &,
		    gcc_unused bool plain_text,
		    Widget &widget) noexcept
{
	const char *s = widget.GetIdPath();
	if (s == nullptr)
		s = "widget";

	return istream_string_new(pool, s);
}

RewriteUriMode
parse_uri_mode(std::string_view) noexcept
{
	return RewriteUriMode::DIRECT;
}

UnusedIstreamPtr
rewrite_widget_uri(gcc_unused struct pool &pool,
		   SharedPoolPtr<WidgetContext>, const StopwatchPtr &,
		   gcc_unused Widget &widget,
		   std::string_view,
		   gcc_unused RewriteUriMode mode,
		   gcc_unused bool stateful,
		   gcc_unused const char *view,
		   gcc_unused const struct escape_class *escape) noexcept
{
	return nullptr;
}

int
main(int argc, char **argv)
try {
	(void)argc;
	(void)argv;

	const ScopeFbPoolInit fb_pool_init;
	PInstance instance;

	FailingResourceLoader resource_loader;

	auto ctx = SharedPoolPtr<WidgetContext>::Make
		(instance.root_pool, instance.event_loop,
		 resource_loader, resource_loader,
		 nullptr,
		 nullptr, nullptr,
		 "localhost:8080",
		 "localhost:8080",
		 "/beng.html",
		 "http://localhost:8080/beng.html",
		 "/beng.html"sv,
		 nullptr,
		 nullptr, nullptr, SessionId{}, nullptr,
		 nullptr);
	auto &widget = ctx->AddRootWidget(MakeRootWidget(instance.root_pool,
							 nullptr));

	auto result =
		processor_process(instance.root_pool, nullptr,
				  OpenFileIstream(instance.event_loop,
						  instance.root_pool,
						  "/dev/stdin"),
				  widget, std::move(ctx), PROCESSOR_CONTAINER);

	StdioSink sink(std::move(result));
	sink.LoopRead();
} catch (...) {
	PrintException(std::current_exception());
	return EXIT_FAILURE;
}
