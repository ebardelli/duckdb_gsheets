#pragma once

#include "sheets/transport/http_client.hpp"

// Forward declarations
namespace duckdb {
class ClientContext;
class HTTPUtil;
} // namespace duckdb

namespace duckdb {
namespace sheets {

// Sends requests through the database's HTTPUtil, i.e. whatever HTTP stack DuckDB
// is configured with: httpfs (curl/httplib) on native builds, the browser's
// fetch/XHR in DuckDB-WASM. Proxy, timeout, retry and logging settings are
// handled by that HTTPUtil rather than by this client.
//
// Core DuckDB's own HTTPUtil only implements GET, so this client is only usable
// once a provider with POST/PUT/DELETE support is installed - see
// SupportsAllMethods and CreateHttpClient.
class DuckDBHttpClient : public IHttpClient {
public:
	explicit DuckDBHttpClient(ClientContext &context);

	HttpResponse Execute(const HttpRequest &request) override;

	// Whether the HTTPUtil implements every method this extension sends.
	static bool SupportsAllMethods(const HTTPUtil &http_util);

private:
	ClientContext &context;
};

} // namespace sheets
} // namespace duckdb
