#include "sheets/transport/duckdb_http_client.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/http_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {
namespace sheets {

DuckDBHttpClient::DuckDBHttpClient(ClientContext &context) : context(context) {
}

bool DuckDBHttpClient::SupportsAllMethods(const HTTPUtil &http_util) {
	// "none" has no client at all and "Built-In" (core's vendored httplib) only
	// implements GET. Every other provider (httpfs, DuckDB-WASM) implements the
	// full set.
	auto name = http_util.GetName();
	return name != "none" && name != "Built-In";
}

HttpResponse DuckDBHttpClient::Execute(const HttpRequest &request) {
	auto &http_util = HTTPUtil::Get(*context.db);
	auto params = http_util.InitializeParameters(context, request.url);

	HTTPHeaders headers;
	std::string content_type = "application/json";
	for (const auto &h : request.headers) {
		if (h.first == "Content-Type") {
			content_type = h.second;
		}
		headers.Insert(h.first, h.second);
	}
	auto body = const_data_ptr_cast(request.body.data());
	auto body_len = request.body.size();

	unique_ptr<HTTPResponse> result;
	switch (request.method) {
	case HttpMethod::GET: {
		GetRequestInfo info(request.url, headers, *params, nullptr, nullptr);
		info.try_request = true;
		result = http_util.Request(info);
		break;
	}
	case HttpMethod::POST: {
		// PostRequestInfo has no content type of its own; providers read it
		// from the headers (and default to application/octet-stream otherwise).
		if (!headers.HasHeader("Content-Type")) {
			headers.Insert("Content-Type", content_type);
		}
		PostRequestInfo info(request.url, headers, *params, body, body_len);
		info.try_request = true;
		result = http_util.Request(info);
		break;
	}
	case HttpMethod::PUT: {
		PutRequestInfo info(request.url, headers, *params, body, body_len, content_type);
		info.try_request = true;
		result = http_util.Request(info);
		break;
	}
	case HttpMethod::DEL: {
		DeleteRequestInfo info(request.url, headers, *params);
		info.try_request = true;
		result = http_util.Request(info);
		break;
	}
	}

	// try_request turns HTTP error statuses into a returned response (callers
	// inspect statusCode themselves), but a transport failure has no status.
	if (result->HasRequestError()) {
		throw duckdb::IOException("HTTP request failed: " + result->GetRequestError());
	}

	HttpResponse response;
	response.statusCode = static_cast<int>(result->status);
	response.body = result->body;
	for (const auto &h : result->headers) {
		response.headers[h.first] = h.second;
	}
	return response;
}

} // namespace sheets
} // namespace duckdb
