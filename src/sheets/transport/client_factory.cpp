#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/http_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"

#include "sheets/transport/client_factory.hpp"
#include "sheets/transport/duckdb_http_client.hpp"
#include "sheets/transport/http_client.hpp"
#ifndef __EMSCRIPTEN__
#include "sheets/transport/httplib_client.hpp"
#include "utils/proxy.hpp"
#endif

namespace duckdb {
namespace sheets {

std::unique_ptr<IHttpClient> CreateHttpClient(ClientContext &ctx) {
	// Prefer DuckDB's own HTTP stack. Core's built-in HTTPUtil is GET-only, so
	// try to autoload httpfs (which replaces it) before deciding.
	if (!DuckDBHttpClient::SupportsAllMethods(HTTPUtil::Get(*ctx.db))) {
		ExtensionHelper::TryAutoLoadExtension(ctx, "httpfs");
	}
	if (DuckDBHttpClient::SupportsAllMethods(HTTPUtil::Get(*ctx.db))) {
		return make_uniq<DuckDBHttpClient>(ctx);
	}

#ifdef __EMSCRIPTEN__
	// DuckDB-WASM installs its own browser-backed HTTPUtil, so this only
	// happens if something replaced it.
	throw InvalidConfigurationException("HTTP provider '%s' can't send the POST/PUT/DELETE requests the gsheets "
	                                    "extension needs",
	                                    HTTPUtil::Get(*ctx.db).GetName());
#else
	// httpfs isn't available (not installed and autoinstall/autoload off, or
	// offline): fall back to our own httplib + OpenSSL client.
	auto proxy_config = GetHttpProxyConfig(ctx);
	return make_uniq<HttpLibClient>(proxy_config);
#endif
}

} // namespace sheets
} // namespace duckdb
