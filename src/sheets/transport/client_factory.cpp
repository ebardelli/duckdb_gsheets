#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/http_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"

#include "sheets/transport/client_factory.hpp"
#include "sheets/transport/duckdb_http_client.hpp"
#include "sheets/transport/http_client.hpp"

namespace duckdb {
namespace sheets {

std::unique_ptr<IHttpClient> CreateHttpClient(ClientContext &ctx) {
	// Requests go through DuckDB's own HTTP stack. Core's built-in HTTPUtil is
	// GET-only (and has no TLS), so on native builds autoload httpfs, which
	// replaces it. DuckDB-WASM installs a browser-backed HTTPUtil of its own.
	if (!DuckDBHttpClient::SupportsAllMethods(HTTPUtil::Get(*ctx.db))) {
		ExtensionHelper::TryAutoLoadExtension(ctx, "httpfs");
	}
	if (!DuckDBHttpClient::SupportsAllMethods(HTTPUtil::Get(*ctx.db))) {
		throw InvalidConfigurationException(
		    "The gsheets extension needs the httpfs extension for HTTPS requests, but it could not be loaded. Run "
		    "INSTALL httpfs; LOAD httpfs; (or enable autoinstall_known_extensions) and try again.");
	}
	return make_uniq<DuckDBHttpClient>(ctx);
}

} // namespace sheets
} // namespace duckdb
