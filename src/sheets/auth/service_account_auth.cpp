#include "json.hpp"
#include "duckdb/common/exception.hpp"

#include "sheets/auth/service_account_auth.hpp"
#include "sheets/transport/http_type.hpp"
#include "sheets/util/encoding.hpp"

#include <random>

#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"

using json = nlohmann::json;

namespace duckdb {
namespace sheets {

// RAII wrapper for mbedtls_pk_context
struct PkContext {
	PkContext() {
		mbedtls_pk_init(&ctx);
	}
	~PkContext() {
		mbedtls_pk_free(&ctx);
	}
	mbedtls_pk_context ctx;
};

// mbedtls needs an RNG for RSA blinding when signing (and for key checks when
// parsing). DuckDB's bundled mbedtls has no CTR_DRBG/entropy module, so use the
// platform's random_device directly.
static int RandomBytes(void *, unsigned char *output, size_t len) {
	static thread_local std::random_device rd;
	for (size_t i = 0; i < len; i++) {
		output[i] = static_cast<unsigned char>(rd());
	}
	return 0;
}

constexpr int TOKEN_TTL = 1800;
constexpr const char *TOKEN_ENDPOINT = "https://oauth2.googleapis.com/token";

std::string ServiceAccountAuth::GetAuthorizationHeader() {
	if (IsExpired()) {
		Refresh();
	}
	return "Bearer " + cachedToken;
}

std::string ServiceAccountAuth::CreateJwt() {
	const char *header = R"({"alg":"RS256","typ":"JWT"})";

	json claimSet;
	std::time_t now = std::time(nullptr);
	claimSet["iss"] = email;                                            // service account email
	claimSet["scope"] = "https://www.googleapis.com/auth/spreadsheets"; // API scope
	claimSet["aud"] = TOKEN_ENDPOINT;                                   // token endpoint
	claimSet["iat"] = now;                                              // issued at
	claimSet["exp"] = now + TOKEN_TTL;                                  // expires in 30 min

	std::string headerB64 = Base64UrlEncode(header);
	std::string claimsB64 = Base64UrlEncode(claimSet.dump());
	std::string signInput = headerB64 + "." + claimsB64;

	auto pem = NormalizePemKey(privateKey);

	// Parse PEM private key (PKCS#8 or PKCS#1); mbedtls needs the PEM's
	// terminating NUL counted in the length
	PkContext pk;
	if (mbedtls_pk_parse_key(&pk.ctx, reinterpret_cast<const unsigned char *>(pem.c_str()), pem.size() + 1, nullptr,
	                         0, RandomBytes, nullptr) != 0) {
		throw duckdb::IOException("Failed to parse private key");
	}
	if (!mbedtls_pk_can_do(&pk.ctx, MBEDTLS_PK_RSA)) {
		throw duckdb::IOException("Private key is not an RSA key");
	}

	unsigned char hash[32];
	if (mbedtls_sha256(reinterpret_cast<const unsigned char *>(signInput.data()), signInput.size(), hash, 0) != 0) {
		throw duckdb::IOException("Failed to hash JWT");
	}

	std::vector<unsigned char> signature(MBEDTLS_PK_SIGNATURE_MAX_SIZE);
	size_t sigLen = 0;
	if (mbedtls_pk_sign(&pk.ctx, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature.data(), signature.size(), &sigLen,
	                    RandomBytes, nullptr) != 0) {
		throw duckdb::IOException("Failed to sign JWT");
	}

	return signInput + "." + Base64UrlEncode(signature.data(), sigLen);
}

void ServiceAccountAuth::ExchangeJwtForToken() {
	std::string jwt = CreateJwt();

	std::string body = "grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer&assertion=" + jwt;

	HttpHeaders headers;
	headers["Content-Type"] = "application/x-www-form-urlencoded";
	HttpResponse response = http.Post(TOKEN_ENDPOINT, headers, body);

	if (response.statusCode != 200) {
		throw duckdb::IOException("Token exchange failed: " + response.body);
	}

	json responseJson;
	try {
		responseJson = json::parse(response.body);
	} catch (const json::exception &) {
		throw duckdb::IOException("Failed to parse token response: " + response.body);
	}

	if (!responseJson.contains("access_token")) {
		throw duckdb::IOException("Token response missing 'access_token': " + response.body);
	}
	cachedToken = responseJson["access_token"].get<std::string>();

	int expiresIn = responseJson.value("expires_in", TOKEN_TTL);
	expirationTime = std::time(nullptr) + expiresIn - 60; // refresh 1 min early
}

bool ServiceAccountAuth::IsExpired() {
	if (cachedToken.empty()) {
		return true;
	}
	std::time_t now = std::time(nullptr);
	return now >= expirationTime;
}

void ServiceAccountAuth::Refresh() {
	ExchangeJwtForToken();
}

} // namespace sheets
} // namespace duckdb
