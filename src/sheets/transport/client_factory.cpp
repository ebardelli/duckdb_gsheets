#include "duckdb/common/helper.hpp"
#include "duckdb/common/http_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"

#include "sheets/transport/client_factory.hpp"
#include "sheets/transport/duckdb_http_client.hpp"
#include "sheets/transport/http_client.hpp"
#include "sheets/transport/httplib_client.hpp"
#include "utils/proxy.hpp"

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

	// httpfs isn't available (not installed and autoinstall/autoload off, or
	// offline): fall back to our own httplib + OpenSSL client.
	auto proxy_config = GetHttpProxyConfig(ctx);
	return make_uniq<HttpLibClient>(proxy_config);
}

} // namespace sheets
} // namespace duckdb
