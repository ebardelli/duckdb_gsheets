#include "catch.hpp"

#include "duckdb/common/exception.hpp"
#include "sheets/auth/bearer_token_auth.hpp"
#include "sheets/auth/oauth_auth.hpp"
#include "sheets/auth/service_account_auth.hpp"
#include "sheets/transport/mock_http_client.hpp"

#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"

// =============================================================================
// BearerTokenAuth Tests
// =============================================================================

TEST_CASE("BearerTokenAuth returns correct header", "[auth]") {
	duckdb::sheets::BearerTokenAuth auth("my-test-token");
	REQUIRE(auth.GetAuthorizationHeader() == "Bearer my-test-token");
}

TEST_CASE("BearerTokenAuth handles empty token", "[auth]") {
	duckdb::sheets::BearerTokenAuth auth("");
	REQUIRE(auth.GetAuthorizationHeader() == "Bearer ");
}

// =============================================================================
// OAuthAuth Tests
// =============================================================================

TEST_CASE("OAuthAuth exchanges the refresh_token for an access token", "[auth]") {
	duckdb::sheets::MockHttpClient mockHttp;

	duckdb::sheets::HttpResponse tokenResponse;
	tokenResponse.statusCode = 200;
	tokenResponse.body = R"({"access_token": "ya29.refreshed-token", "expires_in": 3600})";
	mockHttp.AddResponse(tokenResponse);

	duckdb::sheets::OAuthAuth auth(mockHttp, "my-refresh-token", "my-client-id", "my-client-secret");

	std::string header = auth.GetAuthorizationHeader();
	REQUIRE(header == "Bearer ya29.refreshed-token");

	auto &requests = mockHttp.GetRecordedRequests();
	REQUIRE(requests.size() == 1);
	REQUIRE(requests[0].method == duckdb::sheets::HttpMethod::POST);
	REQUIRE(requests[0].url == "https://oauth2.googleapis.com/token");
	REQUIRE(requests[0].headers.at("Content-Type") == "application/x-www-form-urlencoded");
	REQUIRE(requests[0].body.find("grant_type=refresh_token") != std::string::npos);
	REQUIRE(requests[0].body.find("refresh_token=my-refresh-token") != std::string::npos);
	REQUIRE(requests[0].body.find("client_id=my-client-id") != std::string::npos);
	REQUIRE(requests[0].body.find("client_secret=my-client-secret") != std::string::npos);
}

TEST_CASE("OAuthAuth caches the access token", "[auth]") {
	duckdb::sheets::MockHttpClient mockHttp;

	duckdb::sheets::HttpResponse tokenResponse;
	tokenResponse.statusCode = 200;
	tokenResponse.body = R"({"access_token": "cached-token", "expires_in": 3600})";
	mockHttp.AddResponse(tokenResponse);

	duckdb::sheets::OAuthAuth auth(mockHttp, "refresh-token", "client-id", "client-secret");

	std::string header1 = auth.GetAuthorizationHeader();
	REQUIRE(header1 == "Bearer cached-token");
	REQUIRE(mockHttp.GetRecordedRequests().size() == 1);

	// Second call should use the cached token (no new request).
	std::string header2 = auth.GetAuthorizationHeader();
	REQUIRE(header2 == "Bearer cached-token");
	REQUIRE(mockHttp.GetRecordedRequests().size() == 1);
}

TEST_CASE("OAuthAuth with no access token yet calls the token endpoint exactly once", "[auth]") {
	// A secret created from a passed-in refresh_token has no access token at
	// all: the first GetAuthorizationHeader mints one, and later calls reuse it.
	duckdb::sheets::MockHttpClient mockHttp;

	duckdb::sheets::HttpResponse tokenResponse;
	tokenResponse.statusCode = 200;
	tokenResponse.body = R"({"access_token": "first-token", "expires_in": 3600})";
	mockHttp.AddResponse(tokenResponse);

	duckdb::sheets::OAuthAuth auth(mockHttp, "passed-in-refresh-token", "client-id", "client-secret");
	REQUIRE(mockHttp.GetRecordedRequests().empty());

	REQUIRE(auth.GetAuthorizationHeader() == "Bearer first-token");
	REQUIRE(auth.GetAuthorizationHeader() == "Bearer first-token");
	REQUIRE(auth.GetAuthorizationHeader() == "Bearer first-token");

	auto &requests = mockHttp.GetRecordedRequests();
	REQUIRE(requests.size() == 1);
	REQUIRE(requests[0].url == "https://oauth2.googleapis.com/token");
	REQUIRE(requests[0].body.find("refresh_token=passed-in-refresh-token") != std::string::npos);
}

TEST_CASE("OAuthAuth throws on HTTP error", "[auth]") {
	duckdb::sheets::MockHttpClient mockHttp;

	duckdb::sheets::HttpResponse errorResponse;
	errorResponse.statusCode = 400;
	errorResponse.body = R"({"error": "invalid_grant"})";
	mockHttp.AddResponse(errorResponse);

	duckdb::sheets::OAuthAuth auth(mockHttp, "refresh-token", "client-id", "client-secret");

	REQUIRE_THROWS_AS(auth.GetAuthorizationHeader(), duckdb::IOException);
}

TEST_CASE("OAuthAuth without a reauth callback throws OAuthInvalidGrantException on invalid_grant", "[auth]") {
	duckdb::sheets::MockHttpClient mockHttp;

	duckdb::sheets::HttpResponse errorResponse;
	errorResponse.statusCode = 400;
	errorResponse.body = R"({"error": "invalid_grant"})";
	mockHttp.AddResponse(errorResponse);

	duckdb::sheets::OAuthAuth auth(mockHttp, "refresh-token", "client-id", "client-secret");

	REQUIRE_THROWS_AS(auth.GetAuthorizationHeader(), duckdb::sheets::OAuthInvalidGrantException);
}

TEST_CASE("OAuthAuth does not invoke the reauth callback on a non-invalid_grant error", "[auth]") {
	duckdb::sheets::MockHttpClient mockHttp;

	duckdb::sheets::HttpResponse errorResponse;
	errorResponse.statusCode = 500;
	errorResponse.body = R"({"error": "server_error"})";
	mockHttp.AddResponse(errorResponse);

	bool reauthCalled = false;
	duckdb::sheets::OAuthAuth auth(mockHttp, "refresh-token", "client-id", "client-secret", [&]() {
		reauthCalled = true;
		duckdb::sheets::OAuthTokenResponse result;
		result.access_token = "should-not-be-used";
		result.refresh_token = "should-not-be-used";
		result.expires_in = 3600;
		return result;
	});

	REQUIRE_THROWS_AS(auth.GetAuthorizationHeader(), duckdb::IOException);
	REQUIRE_FALSE(reauthCalled);
}

TEST_CASE("OAuthAuth re-authenticates via the callback on invalid_grant", "[auth]") {
	duckdb::sheets::MockHttpClient mockHttp;

	duckdb::sheets::HttpResponse errorResponse;
	errorResponse.statusCode = 400;
	errorResponse.body = R"({"error": "invalid_grant"})";
	mockHttp.AddResponse(errorResponse);

	bool reauthCalled = false;
	duckdb::sheets::OAuthAuth auth(mockHttp, "dead-refresh-token", "client-id", "client-secret", [&]() {
		reauthCalled = true;
		duckdb::sheets::OAuthTokenResponse result;
		result.access_token = "fresh-access-token";
		result.refresh_token = "fresh-refresh-token";
		result.expires_in = 3600;
		return result;
	});

	std::string header = auth.GetAuthorizationHeader();
	REQUIRE(reauthCalled);
	REQUIRE(header == "Bearer fresh-access-token");

	// Only the one failed refresh request hit the token endpoint - the
	// reauth callback is responsible for its own (mocked-away) HTTP work.
	REQUIRE(mockHttp.GetRecordedRequests().size() == 1);

	// The refreshed token is now cached, so a second call neither refreshes
	// nor re-authenticates again.
	std::string header2 = auth.GetAuthorizationHeader();
	REQUIRE(header2 == "Bearer fresh-access-token");
	REQUIRE(mockHttp.GetRecordedRequests().size() == 1);
}

TEST_CASE("OAuthAuth propagates the reauth callback's failure and re-throws on the next call", "[auth]") {
	duckdb::sheets::MockHttpClient mockHttp;

	duckdb::sheets::HttpResponse errorResponse;
	errorResponse.statusCode = 400;
	errorResponse.body = R"({"error": "invalid_grant"})";
	mockHttp.AddResponse(errorResponse);
	mockHttp.AddResponse(errorResponse);

	duckdb::sheets::OAuthAuth auth(
	    mockHttp, "dead-refresh-token", "client-id", "client-secret",
	    [&]() -> duckdb::sheets::OAuthTokenResponse { throw duckdb::IOException("user cancelled login"); });

	REQUIRE_THROWS_AS(auth.GetAuthorizationHeader(), duckdb::IOException);
	// The failed reauth attempt must not leave the instance stuck thinking a
	// refresh is still in-flight (see the refreshing/refreshCv bookkeeping) -
	// a later call should be able to try again rather than hang.
	REQUIRE_THROWS_AS(auth.GetAuthorizationHeader(), duckdb::IOException);
}

TEST_CASE("OAuthAuth throws on missing access_token", "[auth]") {
	duckdb::sheets::MockHttpClient mockHttp;

	duckdb::sheets::HttpResponse badResponse;
	badResponse.statusCode = 200;
	badResponse.body = R"({"token_type": "Bearer"})"; // Missing access_token
	mockHttp.AddResponse(badResponse);

	duckdb::sheets::OAuthAuth auth(mockHttp, "refresh-token", "client-id", "client-secret");

	REQUIRE_THROWS_AS(auth.GetAuthorizationHeader(), duckdb::IOException);
}

TEST_CASE("OAuthAuth throws on invalid JSON", "[auth]") {
	duckdb::sheets::MockHttpClient mockHttp;

	duckdb::sheets::HttpResponse badResponse;
	badResponse.statusCode = 200;
	badResponse.body = "not valid json";
	mockHttp.AddResponse(badResponse);

	duckdb::sheets::OAuthAuth auth(mockHttp, "refresh-token", "client-id", "client-secret");

	REQUIRE_THROWS_AS(auth.GetAuthorizationHeader(), duckdb::IOException);
}

TEST_CASE("OAuthAuth throws IOException (not an uncaught json::type_error) on a wrong-typed field", "[auth]") {
	// A 200 response whose access_token is present but the wrong JSON type
	// (null here) must still produce the same clean IOException as a parse
	// failure, not an uncaught nlohmann::json::type_error escaping from
	// .get<std::string>().
	duckdb::sheets::MockHttpClient mockHttp;

	duckdb::sheets::HttpResponse badResponse;
	badResponse.statusCode = 200;
	badResponse.body = R"({"access_token": null, "expires_in": 3600})";
	mockHttp.AddResponse(badResponse);

	duckdb::sheets::OAuthAuth auth(mockHttp, "refresh-token", "client-id", "client-secret");

	REQUIRE_THROWS_AS(auth.GetAuthorizationHeader(), duckdb::IOException);
}

// =============================================================================
// ServiceAccountAuth Tests
// =============================================================================

// Test RSA key pair, generated for these tests only (PKCS#8, the format of a
// Google service account key file's private_key). It has to be a real key:
// mbedtls checks each signature it produces against the public key.
static const char TEST_PRIVATE_KEY[] = R"(-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQCVYbj2733Fy2tl
S+EtAvWs6RuUwHYN2Vak0itawpdnArlH4Ip1kHcQKCX7skRiOtgC0/ZLBYA+QqtE
mKj/S28QVFIA+NOtT5mI2J05oZLqICGN8tO3HfSIVY+1XA5lYLccn5Yr1Nnw6dft
2P+BoFAexNu14aC9lYB2DHLFFYUPD1YMSr39zCGU8ddM6jYnxFttQPFif7Slr94n
gsG28oUmjZzPhtdNG5hl7+v1nIiIMuCULztmbm9tohG3E2Ut251NJXrSAse1QF07
nYrDpyUo3comUU7TnWH1fWZpeUh75qrVrlJaJF037elqtT3g+47Y/gsHBJMzgFBs
5Dk0TWjtAgMBAAECggEAEv8AVHaUGekKWTFTCzRLJZfnJ/1phc1TlVYcqUgBlNnl
0dwd/2FFospYOi+rPoqZGZL2yQeEDWbQ/MIlC4H+N6dhXjpGQM3UinBxXfjMA/Xe
Dn0Iq0ql5AhtY/S25/Fql7s7j4tbw6UZDmf/5e4LAj6qafCtTi6KzXg7yBA6gXWS
D9IJsXSKG957sbQWuX3iEmCMzAFrxUP+F6oSsMhht7l6lv1qyEUKf1mogeqb/gIl
RucjWTSsJZqHRJxHUBNcjuxB4lOWfXH9+iZii0K0Jm0BTU8qnWCuL/ur+T0s9huI
h0oLvrOqToB4IgTWkvMb+t+GdL/gA+11YxDhpFTpiQKBgQDGfp43HSiq7AYaqJLo
QEx00B1mEL+sCfHPjQjr/wem1oBlzstzz2hnUacC4MnObiTZdqpnhfB0UQfv0kaX
oMi0JsONgt7rgOKSHGCgTAiRPnBYG+s4U35Shr7jaKmgbXQuT1+wpehZxdzne56/
L89NfQcnUxx2l4zWa7pK2d+OLwKBgQDAqKQQUscb+BetJ5Ky6pU4Ad+PmkxbtX2x
WwIl/OBJRJaDOrn7jW7KeHsLvmk+58vyEoJczh9UTpbpNzLJm7xzvR6CBoo/VDyD
bEQ0kaEtGaK30juTv/laPXpCOP0seJeDImVkbie1erR3gXU1pvwADeVHOBrVaXe0
uGqZuEjvowKBgDugd5ioyrmlkyPY9eCMyCrjBaHOcDWW+yvU+xQhlUPZ9dIfb+CH
Mp6u2ZO0wQqg7bLhwci9ZIEcglzZdtk/tGrhpibRZHOsUOOBYMlmbcBr2EWvhVZz
RmQ2FAuuOh52IstyjpU7EXQ+SuDCsupNy2FEqKx4e/loBye2Ts0Da/ODAoGAIUV+
f+srw6LGw/qPlSJ5/qAkMbKd+UCQom5rIpBXU4/qxmL/93dnaMQBy/doSZrSV83m
gYx/UJn56fDEbpjcpMhGWoIyodk0UiYCaWm1AgrpVQGTlGKU94aidIlO/70bvWPV
StDTPr4gKHtM+KUL5B6COBhyWeTK7YTjtARlZC0CgYEAtiXLWdzG6Vpwr+B8ORR4
u886UQjBK/HDd5fZbGc4Z2JeFdq9pJ2rr4qcEcN30DKf5/2mp9l79RkdJccR+vNB
2t5Ri07D5xqWemnaajt+DeEcuDA2zY0IB2xAcT62GAgGfZ7kPBGtNfqDQYHqjIyo
Ep9D54FfKriVGb9sBNyoAfg=
-----END PRIVATE KEY-----
)";

static const char TEST_PUBLIC_KEY[] = R"(-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAlWG49u99xctrZUvhLQL1
rOkblMB2DdlWpNIrWsKXZwK5R+CKdZB3ECgl+7JEYjrYAtP2SwWAPkKrRJio/0tv
EFRSAPjTrU+ZiNidOaGS6iAhjfLTtx30iFWPtVwOZWC3HJ+WK9TZ8OnX7dj/gaBQ
HsTbteGgvZWAdgxyxRWFDw9WDEq9/cwhlPHXTOo2J8RbbUDxYn+0pa/eJ4LBtvKF
Jo2cz4bXTRuYZe/r9ZyIiDLglC87Zm5vbaIRtxNlLdudTSV60gLHtUBdO52Kw6cl
KN3KJlFO051h9X1maXlIe+aq1a5SWiRdN+3parU94PuO2P4LBwSTM4BQbOQ5NE1o
7QIDAQAB
-----END PUBLIC KEY-----
)";

// Base64url (no padding) decode, for checking JWT signatures
static std::string Base64UrlDecode(std::string input) {
	for (auto &c : input) {
		if (c == '-') {
			c = '+';
		} else if (c == '_') {
			c = '/';
		}
	}
	while (input.size() % 4 != 0) {
		input += '=';
	}
	std::string out(input.size(), '\0');
	size_t out_len = 0;
	REQUIRE(mbedtls_base64_decode(reinterpret_cast<unsigned char *>(&out[0]), out.size(), &out_len,
	                              reinterpret_cast<const unsigned char *>(input.data()), input.size()) == 0);
	out.resize(out_len);
	return out;
}

TEST_CASE("ServiceAccountAuth creates valid JWT structure", "[auth]") {
	duckdb::sheets::MockHttpClient mockHttp;

	// Queue a successful token response
	duckdb::sheets::HttpResponse tokenResponse;
	tokenResponse.statusCode = 200;
	tokenResponse.body = R"({"access_token": "ya29.test-token", "expires_in": 3600})";
	mockHttp.AddResponse(tokenResponse);

	duckdb::sheets::ServiceAccountAuth auth(mockHttp, "test@project.iam.gserviceaccount.com", TEST_PRIVATE_KEY);

	std::string header = auth.GetAuthorizationHeader();

	// Should return Bearer token
	REQUIRE(header == "Bearer ya29.test-token");

	// Verify the HTTP request was made correctly
	auto &requests = mockHttp.GetRecordedRequests();
	REQUIRE(requests.size() == 1);
	REQUIRE(requests[0].method == duckdb::sheets::HttpMethod::POST);
	REQUIRE(requests[0].url == "https://oauth2.googleapis.com/token");
	REQUIRE(requests[0].headers.at("Content-Type") == "application/x-www-form-urlencoded");

	// Body should contain JWT assertion
	REQUIRE(requests[0].body.find("grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer") != std::string::npos);
	REQUIRE(requests[0].body.find("assertion=") != std::string::npos);
}

TEST_CASE("ServiceAccountAuth JWT has correct structure", "[auth]") {
	duckdb::sheets::MockHttpClient mockHttp;

	duckdb::sheets::HttpResponse tokenResponse;
	tokenResponse.statusCode = 200;
	tokenResponse.body = R"({"access_token": "test-token", "expires_in": 3600})";
	mockHttp.AddResponse(tokenResponse);

	duckdb::sheets::ServiceAccountAuth auth(mockHttp, "test@example.com", TEST_PRIVATE_KEY);
	auth.GetAuthorizationHeader();

	auto &requests = mockHttp.GetRecordedRequests();
	std::string body = requests[0].body;

	// Extract JWT from assertion=<jwt>
	size_t pos = body.find("assertion=");
	REQUIRE(pos != std::string::npos);
	std::string jwt = body.substr(pos + 10);

	// JWT should have 3 parts separated by dots
	int dotCount = 0;
	for (char c : jwt) {
		if (c == '.') {
			dotCount++;
		}
	}
	REQUIRE(dotCount == 2);

	// The signature is RS256 over "<header>.<claims>" and verifies against the public key
	size_t sigPos = jwt.rfind('.');
	std::string signInput = jwt.substr(0, sigPos);
	std::string signature = Base64UrlDecode(jwt.substr(sigPos + 1));

	unsigned char hash[32];
	REQUIRE(mbedtls_sha256(reinterpret_cast<const unsigned char *>(signInput.data()), signInput.size(), hash, 0) == 0);

	mbedtls_pk_context pub;
	mbedtls_pk_init(&pub);
	REQUIRE(mbedtls_pk_parse_public_key(&pub, reinterpret_cast<const unsigned char *>(TEST_PUBLIC_KEY),
	                                    sizeof(TEST_PUBLIC_KEY)) == 0);
	int verify = mbedtls_pk_verify(&pub, MBEDTLS_MD_SHA256, hash, sizeof(hash),
	                               reinterpret_cast<const unsigned char *>(signature.data()), signature.size());
	mbedtls_pk_free(&pub);
	REQUIRE(verify == 0);
}

TEST_CASE("ServiceAccountAuth caches token", "[auth]") {
	duckdb::sheets::MockHttpClient mockHttp;

	duckdb::sheets::HttpResponse tokenResponse;
	tokenResponse.statusCode = 200;
	tokenResponse.body = R"({"access_token": "cached-token", "expires_in": 3600})";
	mockHttp.AddResponse(tokenResponse);

	duckdb::sheets::ServiceAccountAuth auth(mockHttp, "test@example.com", TEST_PRIVATE_KEY);

	// First call should make HTTP request
	std::string header1 = auth.GetAuthorizationHeader();
	REQUIRE(header1 == "Bearer cached-token");
	REQUIRE(mockHttp.GetRecordedRequests().size() == 1);

	// Second call should use cached token (no new request)
	std::string header2 = auth.GetAuthorizationHeader();
	REQUIRE(header2 == "Bearer cached-token");
	REQUIRE(mockHttp.GetRecordedRequests().size() == 1); // Still 1, not 2
}

TEST_CASE("ServiceAccountAuth throws on HTTP error", "[auth]") {
	duckdb::sheets::MockHttpClient mockHttp;

	duckdb::sheets::HttpResponse errorResponse;
	errorResponse.statusCode = 401;
	errorResponse.body = R"({"error": "invalid_client"})";
	mockHttp.AddResponse(errorResponse);

	duckdb::sheets::ServiceAccountAuth auth(mockHttp, "test@example.com", TEST_PRIVATE_KEY);

	REQUIRE_THROWS_AS(auth.GetAuthorizationHeader(), duckdb::IOException);
}

TEST_CASE("ServiceAccountAuth throws on missing access_token", "[auth]") {
	duckdb::sheets::MockHttpClient mockHttp;

	duckdb::sheets::HttpResponse badResponse;
	badResponse.statusCode = 200;
	badResponse.body = R"({"token_type": "Bearer"})"; // Missing access_token
	mockHttp.AddResponse(badResponse);

	duckdb::sheets::ServiceAccountAuth auth(mockHttp, "test@example.com", TEST_PRIVATE_KEY);

	REQUIRE_THROWS_AS(auth.GetAuthorizationHeader(), duckdb::IOException);
}

TEST_CASE("ServiceAccountAuth throws on invalid JSON", "[auth]") {
	duckdb::sheets::MockHttpClient mockHttp;

	duckdb::sheets::HttpResponse badResponse;
	badResponse.statusCode = 200;
	badResponse.body = "not valid json";
	mockHttp.AddResponse(badResponse);

	duckdb::sheets::ServiceAccountAuth auth(mockHttp, "test@example.com", TEST_PRIVATE_KEY);

	REQUIRE_THROWS_AS(auth.GetAuthorizationHeader(), duckdb::IOException);
}
