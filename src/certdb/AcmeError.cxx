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

#include "AcmeError.hxx"
#include "json/String.hxx"
#include "util/Exception.hxx"

#include <boost/json.hpp>

static std::string
MakeAcmeErrorMessage(const boost::json::object &error) noexcept
{
	const auto *detail = error.if_contains("detail");
	if (detail == nullptr)
		return "Server error";

	std::string msg = "Server error: ";
	msg.append(detail->as_string());
	return msg;
}

AcmeError::AcmeError(const boost::json::object &error)
	:std::runtime_error(MakeAcmeErrorMessage(error)),
	 type(Json::GetString(error.if_contains("type")))
{
}

bool
IsAcmeErrorType(std::exception_ptr ep, const char *type) noexcept
{
	const auto *e = FindNested<AcmeError>(ep);
	return e != nullptr && e->GetType() == type;
}

bool
IsAcmeUnauthorizedError(std::exception_ptr ep) noexcept
{
	return IsAcmeErrorType(ep, "urn:acme:error:unauthorized");
}
