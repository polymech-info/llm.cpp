#pragma once

/*
	exception.h : liboai exception header.
		This header file provides declarations for exception
		directives for handling exceptions thrown by liboai
		component classes.
*/

#include <iostream>
#include <exception>
#include <memory>

#if defined(LIBOAI_DEBUG)
	#define _liboai_dbg(fmt, ...) printf(fmt, __VA_ARGS__);
#endif

namespace liboai {
	namespace exception {
		enum class EType : uint8_t {
			E_FAILURETOPARSE,
			E_BADREQUEST,
			E_APIERROR,
			E_RATELIMIT,
			E_CONNECTIONERROR,
			E_FILEERROR,
			E_CURLERROR
		};

		constexpr const char* _etype_strs_[7] = {
			"E_FAILURETOPARSE:0x00",
			"E_BADREQUEST:0x01",
			"E_APIERROR:0x02",
			"E_RATELIMIT:0x03",
			"E_CONNECTIONERROR:0x04",
			"E_FILEERROR:0x05",
			"E_CURLERROR:0x06"
		};

		class OpenAIException : public std::exception {
			public:
				OpenAIException() = default;
                                OpenAIException(const OpenAIException& rhs) noexcept
					: error_type_(rhs.error_type_),
					  data_(rhs.data_),
					  locale_(rhs.locale_),
					  http_status_code_(rhs.http_status_code_),
					  response_body_(rhs.response_body_),
					  response_url_(rhs.response_url_),
					  response_reason_(rhs.response_reason_) {
					this->fmt_str_ = (this->locale_ + ": " + this->data_ + " (" + this->GetETypeString(this->error_type_) + ")");
				}
				OpenAIException(OpenAIException&& rhs) noexcept
					: error_type_(rhs.error_type_),
					  data_(std::move(rhs.data_)),
					  locale_(std::move(rhs.locale_)),
					  http_status_code_(rhs.http_status_code_),
					  response_body_(std::move(rhs.response_body_)),
					  response_url_(std::move(rhs.response_url_)),
					  response_reason_(std::move(rhs.response_reason_)) {
					this->fmt_str_ = (this->locale_ + ": " + this->data_ + " (" + this->GetETypeString(this->error_type_) + ")");
				}
				OpenAIException(std::string_view data,
								EType error_type,
								std::string_view locale,
								long http_status_code = 0,
								std::string_view response_body = {},
								std::string_view response_url = {},
								std::string_view response_reason = {}) noexcept
					: error_type_(error_type),
					  data_(data),
					  locale_(locale),
					  http_status_code_(http_status_code),
					  response_body_(response_body),
					  response_url_(response_url),
					  response_reason_(response_reason) {
					this->fmt_str_ = (this->locale_ + ": " + this->data_ + " (" + this->GetETypeString(this->error_type_) + ")");
				}

				const char* what() const noexcept override {
					return this->fmt_str_.c_str();
				}

				constexpr const char* GetETypeString(EType type) const noexcept {
					return _etype_strs_[static_cast<uint8_t>(type)];
				}

				long HttpStatusCode() const noexcept {
					return this->http_status_code_;
				}

				const std::string& ResponseBody() const noexcept {
					return this->response_body_;
				}

				const std::string& ResponseUrl() const noexcept {
					return this->response_url_;
				}

				const std::string& ResponseReason() const noexcept {
					return this->response_reason_;
				}

			private:
				EType error_type_;
				std::string data_, locale_, fmt_str_;
				long http_status_code_ = 0;
				std::string response_body_, response_url_, response_reason_;
		};

		class OpenAIRateLimited : public std::exception {
			public:
				OpenAIRateLimited() = default;
				OpenAIRateLimited(const OpenAIRateLimited& rhs) noexcept
					: error_type_(rhs.error_type_), data_(rhs.data_), locale_(rhs.locale_) { this->fmt_str_ = (this->locale_ + ": " + this->data_ + " (" + this->GetETypeString(this->error_type_) + ")"); }
				OpenAIRateLimited(OpenAIRateLimited&& rhs) noexcept
					: error_type_(rhs.error_type_), data_(std::move(rhs.data_)), locale_(std::move(rhs.locale_)) { this->fmt_str_ = (this->locale_ + ": " + this->data_ + " (" + this->GetETypeString(this->error_type_) + ")"); }
				OpenAIRateLimited(std::string_view data, EType error_type, std::string_view locale) noexcept
					: error_type_(error_type), data_(data), locale_(locale) { this->fmt_str_ = (this->locale_ + ": " + this->data_ + " (" + this->GetETypeString(this->error_type_) + ")"); }

				const char* what() const noexcept override {
					return this->fmt_str_.c_str();
				}

				constexpr const char* GetETypeString(EType type) const noexcept {
					return _etype_strs_[static_cast<uint8_t>(type)];
				}

			private:
				EType error_type_;
				std::string data_, locale_, fmt_str_;
		};
	}
}
