---
title: DuckDB GSheets
description: A DuckDB extension for reading and writing Google Sheets with SQL.
hide_title: true
---

<h1 class="markdown flex items-center gap-2"><img src="icon-512.png" style="height: 1em;"/> DuckDB GSheets</h1>

A DuckDB extension for reading and writing Google Sheets with SQL.

_Note: This is a fork of the [DuckDB GSheets extension](https://duckdb-gsheets.com) ([source](https://github.com/evidence-dev/duckdb_gsheets)) maintained by [Evidence](https://evidence.dev). It publishes builds for DuckDB 2.0 until those are available upstream. This project is not affiliated with Google or DuckDB._

## Install

```sql
INSTALL gsheets FROM community;
LOAD gsheets;
```

The community build is maintained upstream and supports [DuckDB](https://duckdb.org/docs/installation) versions up to 1.5.6. For DuckDB 2.0, install from this fork instead (see below).

### DuckDB 2.0 builds

Builds for DuckDB 2.0 are published from [ebardelli/duckdb_gsheets](https://github.com/ebardelli/duckdb_gsheets) as a self-hosted, signed [extension repository](https://duckdb.org/docs/2.0/extensions/extension_repositories.html):

```sql
SET allow_extension_repositories = 'allowed';
CREATE EXTENSION REPOSITORY gsheets_fork WITH PREFIX 'https://ebardelli.github.io/duckdb_gsheets/extensions';
INSTALL AND LOAD gsheets FROM gsheets_fork;
```

`CREATE EXTENSION REPOSITORY` prints the fingerprint of the signing key it pinned. Check that it matches `sha256:30a99a8ad18afde28756024575259ce6a65eb53232d91790efaff11035fdcacc`. The repository only needs to be created once; it persists across sessions. In later sessions, load the extension with `LOAD gsheets FROM gsheets_fork;`. To install a specific release, use `FORCE INSTALL gsheets FROM gsheets_fork VERSION 'vX.Y.Z';`.

## Usage

### Authenticate

```sql
-- Authenticate with Google Account in the browser (default)
CREATE SECRET (TYPE gsheet);

-- The default flow above issues a short-lived access token only: once it
-- expires (about an hour) or DuckDB restarts, you'll need to log in again.
-- To get a secret that silently re-authenticates itself instead, register
-- your own OAuth client in Google Cloud Console (Desktop app type; no
-- redirect URI needs to be added - loopback addresses are allowed
-- automatically) and supply both its client ID and client secret:
CREATE SECRET (
    TYPE gsheet,
    PROVIDER oauth,
    CLIENT_ID '<your_oauth_client_id>',
    CLIENT_SECRET '<your_oauth_client_secret>'
);

-- You can also supply just CLIENT_ID (no CLIENT_SECRET) to use your own
-- OAuth app with the default browser login above, without gaining
-- re-authentication.

-- OR, on a machine with no browser (e.g. a scheduled pipeline), pass in a
-- refresh token you already have, together with the OAuth client it was
-- issued to. No login happens. (See "Pipelines and headless machines" below.)
CREATE SECRET (
    TYPE gsheet,
    PROVIDER oauth,
    CLIENT_ID '<your_oauth_client_id>',
    CLIENT_SECRET '<your_oauth_client_secret>',
    REFRESH_TOKEN '<your_refresh_token>'
);


-- OR create a secret with your Google API access token (boring, see below guide)
CREATE SECRET (
    TYPE gsheet,
    PROVIDER access_token,
    TOKEN '<your_token>'
);


-- OR create a non-expiring JSON secret with your Google API private key
-- (This enables use in non-interactive workflows like data pipelines)
-- (see "Getting a Google API Access Private Key" below)
CREATE SECRET (
    TYPE gsheet,
    PROVIDER key_file,
    FILEPATH '<path_to_JSON_file_with_private_key>'
);

-- OR by passing the secret directly
CREATE SECRET (
    TYPE gsheet,
    PROVIDER key_file,
    EMAIL '<service_account_email>',
    SECRET '<private_key>'
);
```

### How the browser login works

`CREATE SECRET (TYPE gsheet)` (the `oauth` provider) signs you in with Google OAuth 2.0 in your browser:

1. DuckDB opens Google's sign-in page in your default browser and prints the same URL in case the browser doesn't open. It asks for access to Google Sheets only (`https://www.googleapis.com/auth/spreadsheets`).
2. A temporary listener on `http://localhost:8765` waits for Google to redirect back after you sign in. That completes the login without any copy and paste. Port 8765 must be free; if it isn't, `CREATE SECRET` fails with an error saying so.
3. The login times out after 5 minutes. Press Ctrl+C to cancel it sooner.

**Remote or headless machines.** If DuckDB runs somewhere your browser's redirect can't reach (over SSH, in a container), the redirect to `localhost` fails. When DuckDB is reading from an interactive terminal, it also offers to accept the redirect URL instead: after signing in, copy the full URL from the browser's address bar (it will show an error page, which is expected), paste it into the terminal and press Enter. A `state` value in the URL is checked, so a URL from a different login is rejected.

**Which flow you get** depends on the options you pass:

| Options | Flow | Result |
|---|---|---|
| none | Implicit grant, built-in OAuth app | Access token only. It expires after about an hour, and you sign in again. |
| `CLIENT_ID` | Implicit grant, your OAuth app | Same as above, using your app. |
| `CLIENT_ID` + `CLIENT_SECRET` | Authorization code with PKCE, your OAuth app | Access token **and** refresh token. Expired access tokens are renewed in the background without a browser. |
| `CLIENT_ID` + `CLIENT_SECRET` + `REFRESH_TOKEN` | No browser login, your OAuth app | Uses the refresh token you pass in. Access tokens are renewed in the background. If the refresh token is revoked, queries fail with an error. |

With a refresh token, the extension renews the access token about a minute before it expires. If Google revokes the refresh token (for example, you remove the app's access in your Google account, or the token goes unused for a long time), the next `read_gsheet` call opens the browser login again and saves the new tokens back to the secret. This also updates a `PERSISTENT` secret. `COPY ... TO` never prompts and fails with an error instead; run a `read_gsheet` query or re-create the secret to sign in again. A secret created with `REFRESH_TOKEN` never opens the browser either: every query fails with an error until you create the secret again with a new refresh token.

Tokens, the refresh token and the client secret are redacted when you list secrets. A `PERSISTENT` secret is stored on disk in DuckDB's secret directory, so treat that directory as sensitive.

If your own OAuth app's publishing status in Google Cloud Console is still "Testing", Google expires its refresh tokens after 7 days, so you'll be asked to sign in about once a week. Publish the app to avoid this.

For a non-interactive setup such as a scheduled pipeline, see [Pipelines and headless machines](#pipelines-and-headless-machines) below. A service account (`key_file` provider) also works, but every sheet must then be shared with the service account's email address.

### Pipelines and headless machines

A pipeline can read and write sheets as a real Google user, with no browser, by passing a refresh token to `CREATE SECRET`. Sheets then only need to be shared with that user. You sign in once on a desktop to get the refresh token:

1. **Create an OAuth client.** In Google Cloud Console, create an OAuth client of type *Desktop app*. On the OAuth consent screen, publish the app, or set its user type to *Internal* if your Google Workspace organization allows it. While the app's publishing status is "Testing", Google expires its refresh tokens after 7 days.

2. **Sign in once on a desktop.** Start the DuckDB CLI with `-unredacted`, which allows secrets to be printed. This can only be set when DuckDB starts, not with `SET` later. Then create a secret with your client ID and secret and sign in in the browser:

   ```sh
   duckdb -unredacted
   ```

   ```sql
   CREATE SECRET gsheet_login (
       TYPE gsheet,
       PROVIDER oauth,
       CLIENT_ID '<your_oauth_client_id>',
       CLIENT_SECRET '<your_oauth_client_secret>'
   );
   ```

3. **Read the refresh token back.** In the same session:

   ```sql
   SELECT regexp_extract(secret_string, 'refresh_token=([^;]*)', 1) AS refresh_token
   FROM duckdb_secrets(redact := false)
   WHERE name = 'gsheet_login';
   ```

   Without `-unredacted`, this fails with `Displaying unredacted secrets is disabled`.

4. **Use it in the pipeline.** Store the client ID, client secret and refresh token in your pipeline's secret store, and create the secret at the start of each run:

   ```sql
   CREATE SECRET (
       TYPE gsheet,
       PROVIDER oauth,
       CLIENT_ID '<your_oauth_client_id>',
       CLIENT_SECRET '<your_oauth_client_secret>',
       REFRESH_TOKEN '<your_refresh_token>'
   );
   ```

   `CREATE SECRET` doesn't contact Google. The refresh token is first used, and checked, by the first query that reads or writes a sheet.

Keep in mind:

- Every run reads and writes as the user who signed in. If that account is suspended, or its password changes (under some Workspace policies, this revokes refresh tokens), the pipeline fails until someone signs in again and updates the stored refresh token. A dedicated account for the pipeline avoids tying it to one person.
- The refresh token can access every sheet that user can open, not just the ones the pipeline uses. Treat it like a password. The extension only ever asks for the Sheets scope (`https://www.googleapis.com/auth/spreadsheets`).

### HTTP Proxy

See [HTTP Proxy documentation](https://duckdb.org/docs/stable/core_extensions/httpfs/https#http-proxy).

You can add an HTTP proxy using the Secrets Manager:

```sql
CREATE SECRET http_proxy (
    TYPE http,
    HTTP_PROXY 'http_proxy_url',
    HTTP_PROXY_USERNAME 'username',
    HTTP_PROXY_PASSWORD 'password'
);
```

Alternatively, you can add it via configuration options:

```sql
SET http_proxy = 'http_proxy_url';
SET http_proxy_username = 'username';
SET http_proxy_password = 'password';
```

### Read

```sql
-- Read a spreadsheet by full URL
FROM read_gsheet('https://docs.google.com/spreadsheets/d/11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8/edit');

-- Read a spreadsheet by full URL, implicitly
FROM 'https://docs.google.com/spreadsheets/d/11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8/edit';

-- Read a spreadsheet by spreadsheet id
FROM read_gsheet('11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8');

-- Read a spreadsheet with no header row
SELECT * FROM read_gsheet('11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8', header=false);

-- Read all values in as varchar, skipping type inference
SELECT * FROM read_gsheet('11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8', all_varchar=true);

-- Read a sheet other than the first sheet using the sheet name
SELECT * FROM read_gsheet('11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8', sheet='Sheet2');

-- Read a spreadsheet using a specific range
SELECT * FROM read_gsheet('11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8', sheet='Sheet1', range='B1:C7');
    -- or using A1 notation
SELECT * FROM read_gsheet('11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8', sheet='Sheet1!B1:C7');
    -- or from range in URL
SELECT * FROM read_gsheet('https://docs.google.com/spreadsheets/d/11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8/edit?gid=0#gid=0&range=B1:C7');

-- Read a sheet other than the first sheet using the sheet id in the URL
SELECT * FROM read_gsheet('https://docs.google.com/spreadsheets/d/11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8/edit?gid=644613997#gid=644613997');
```

### Write

```sql
-- Write a spreadsheet from a table by spreadsheet id
COPY <table_name> TO '11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8' (FORMAT gsheet);

-- Write a spreadsheet from a table by full URL
COPY <table_name> TO 'https://docs.google.com/spreadsheets/d/11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8/edit?usp=sharing' (FORMAT gsheet);

-- Create a sheet if it doesn't already exist
COPY <table_name> TO 'https://docs.google.com/spreadsheets/d/11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8/edit' (FORMAT gsheet, sheet 'Woot', create_if_not_exists true);

-- Write a spreadsheet to a specific sheet using the sheet id in the URL
COPY <table_name> TO 'https://docs.google.com/spreadsheets/d/11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8/edit?gid=1295634987#gid=1295634987' (FORMAT gsheet);

-- Write a spreadsheet to a specific range using the range in the URL
copy <table_name> to 'https://docs.google.com/spreadsheets/d/11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8/edit?gid=1385451074#gid=1385451074&range=C6:E10' (format gsheet);

-- Write a spreadsheet to a specific sheet with the sheet parameter
-- NOTE: A sheet parameter will take precedence over the query string
copy <table_name>
to 'https://docs.google.com/spreadsheets/d/11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8/edit'
(format gsheet, sheet 'Woot');

-- Write a spreadsheet to a specific range with the range parameter
-- NOTE: A range parameter will take precedence over the query string
copy <table_name>
to 'https://docs.google.com/spreadsheets/d/11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8/edit'
(format gsheet, sheet 'Woot', range 'B2:C10000');

-- Only overwrite the range that is being written to
copy <table_name>
to 'https://docs.google.com/spreadsheets/d/11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8/edit'
(format gsheet, sheet 'Woot', range 'B2:C10000', overwrite_range TRUE);

-- Overwrite the entire sheet (this is the default)
copy <table_name>
to 'https://docs.google.com/spreadsheets/d/11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8/edit'
(format gsheet, sheet 'Woot', range 'B2:C10000', overwrite_sheet TRUE);

-- Append below existing data
-- (by default, no header will be included)
copy <table_name>
to 'https://docs.google.com/spreadsheets/d/11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8/edit'
(format gsheet, sheet 'Woot', range 'B2:C10000', overwrite_sheet FALSE, overwrite_range FALSE);

-- Append below existing data, but force the output of a header
copy <table_name>
to 'https://docs.google.com/spreadsheets/d/11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8/edit'
(format gsheet, sheet 'Woot', range 'B2:C10000', overwrite_sheet FALSE, overwrite_range FALSE, header TRUE);

-- Write with no header
copy <table_name>
to 'https://docs.google.com/spreadsheets/d/11QdEasMWbETbFVxry-SsD8jVcdYIT1zBQszcF84MdE8/edit'
(format gsheet, header FALSE);
```

## Getting a Google API Access Token

To connect DuckDB to Google Sheets via an access token, you’ll need to create a Service Account through the Google API, and use it to generate an access token:

1. Navigate to the [Google API Console](https://console.developers.google.com/apis/library).
2. Create a new project.
3. Search for the Google Sheets API and enable it.
4. In the left-hand navigation, go to the **Credentials** tab.
5. Click **+ Create Credentials** and select **Service Account**.
6. Name the Service Account and assign it the **Owner** role for your project. Click **Done** to save.
7. From the **Service Accounts** page, click on the Service Account you just created.
8. Go to the **Keys** tab, then click **Add Key** > **Create New Key**.
9. Choose **JSON**, then click **Create**. The JSON file will download automatically.
10. Download and install the [gcloud CLI](https://cloud.google.com/sdk/docs/install).
11. Run the following command to login to the gcloud CLI with the Service Account using the newly created JSON file
    ```bash
    gcloud auth activate-service-account --key-file /path/to/key/file
    ```
12. Run the following command to generate an access token:
    ```bash
    gcloud auth print-access-token --scopes=https://www.googleapis.com/auth/spreadsheets
    ```
13. Open your Google Sheet and share it with the Service Account email.
14. Run DuckDB and load the extension

This token will periodically expire - you can re-run the above command again to generate a new one.

## Getting a Google API Access Private Key

Follow steps 1-9 above to get a JSON file with your private key inside.

Include the path to the file as the `FILEPATH` parameter when creating a secret.
Ex: `CREATE SECRET (TYPE gsheet, PROVIDER key_file, FILEPATH '<path_to_JSON_file_with_private_key>');`

You can skip steps 10, 11, and 12 since this extension will convert from your JSON file to a token on your behalf!
The contents of the JSON file will be stored in the secret, as will the temporary token.

Follow steps 13 and 14.

This private key by default will not expire. Use caution with it.

This will also require an additional API request approximately every 30 minutes.

## Limitations / Known Issues

- In DuckDB WASM, the browser login (`PROVIDER oauth` without a `REFRESH_TOKEN`) is not available, since it needs a local listener for the redirect. Use `PROVIDER access_token`, `PROVIDER key_file` with `EMAIL` and `SECRET`, or `PROVIDER oauth` with `CLIENT_ID`, `CLIENT_SECRET` and a `REFRESH_TOKEN`.
- Google Sheets has a limit of 10,000,000 cells per spreadsheet.
- Sheets must already exist to COPY TO them.

## Support

If you are having problems, find a bug, or have an idea for an improvement, please [file an issue on GitHub](https://github.com/evidence-dev/duckdb_gsheets).
