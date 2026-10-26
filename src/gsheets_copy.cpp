#include <utility>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/statement/copy_statement.hpp"
#include "duckdb/planner/binder.hpp"

#include "gsheets_copy.hpp"
#include "gsheets_utils.hpp"

#include "utils/options.hpp"

#include "sheets/auth_factory.hpp"
#include "sheets/client.hpp"
#include "sheets/exception.hpp"
#include "sheets/range.hpp"
#include "sheets/transport/client_factory.hpp"
#include "sheets/types.hpp"

namespace duckdb {

// DuckDB checks every COPY TO target against the filesystem before handing it to the copy function, and a
// docs.google.com URL is claimed by httpfs, which cannot answer those checks. Move the URL's gid and range into
// options and hand core the bare spreadsheet ID instead, which it treats as a plain (nonexistent) local path.
BoundStatement GSheetCopyFunction::GSheetCopyPlan(Binder &binder, CopyStatement &stmt) {
	auto &copy_info = *stmt.info;
	auto &file_path = copy_info.file_path;

	auto gid = extract_sheet_id(file_path);
	if (!gid.empty() && copy_info.options.find(Identifier("gid")) == copy_info.options.end()) {
		copy_info.options[Identifier("gid")] = {Value(gid)};
	}
	auto range = extract_sheet_range(file_path);
	if (!range.empty() && copy_info.options.find(Identifier("range")) == copy_info.options.end()) {
		copy_info.options[Identifier("range")] = {Value(range)};
	}
	file_path = extract_spreadsheet_id(file_path);

	copy_info.format = WRITE_FORMAT;
	return binder.Bind(stmt);
}

CopyFunction GSheetCopyFunction::GetPlanFunction() {
	CopyFunction function("gsheet");
	function.plan = GSheetCopyPlan;
	return function;
}

GSheetCopyFunction::GSheetCopyFunction() : CopyFunction(WRITE_FORMAT) {
	copy_to_bind = GSheetWriteBind;
	copy_to_initialize_global = GSheetWriteInitializeGlobal;
	copy_to_initialize_local = GSheetWriteInitializeLocal;
	copy_to_sink = GSheetWriteSink;
	// Rows are appended in sink, but DuckDB 2.0's batched and file lifecycle paths require these callbacks
	copy_to_combine = GSheetWriteCombine;
	copy_to_finalize = GSheetWriteFinalize;
}

unique_ptr<FunctionData> GSheetCopyFunction::GSheetWriteBind(ClientContext &context, CopyFunctionBindInput &input,
                                                             const vector<Identifier> &names,
                                                             const vector<LogicalType> &sql_types) {
	vector<string> column_names;
	column_names.reserve(names.size());
	for (auto &name : names) {
		column_names.push_back(name.GetIdentifierName());
	}

	string file_path = input.info.file_path;
	auto options = input.info.options;

	auto sheet = duckdb::sheets::GetStringOption(options, "sheet");
	auto gid = duckdb::sheets::GetStringOption(options, "gid");
	auto range = duckdb::sheets::GetStringOption(options, "range");
	bool overwrite_sheet = duckdb::sheets::GetBoolOption(options, "overwrite_sheet", true).first;
	bool overwrite_range = duckdb::sheets::GetBoolOption(options, "overwrite_range", false).first;
	bool create_if_not_exists = duckdb::sheets::GetBoolOption(options, "create_if_not_exists", false).first;

	auto header_result = duckdb::sheets::GetBoolOption(options, "header", true);
	bool header = header_result.second ? header_result.first : (overwrite_range || overwrite_sheet);

	if (create_if_not_exists && sheet.empty()) {
		throw BinderException("Must provide sheet name");
	}

	return make_uniq<GSheetWriteBindData>(file_path, sql_types, column_names, sheet, gid, range, overwrite_sheet,
	                                      overwrite_range, create_if_not_exists, header);
}

unique_ptr<GlobalFunctionData> GSheetCopyFunction::GSheetWriteInitializeGlobal(ClientContext &context,
                                                                               FunctionData &bind_data,
                                                                               const string &file_path) {
	auto options = bind_data.Cast<GSheetWriteBindData>().options;

	std::string spreadsheet_id = extract_spreadsheet_id(file_path);
	std::string sheet_name;
	std::string sheet_range = options.range;

	// Initialize client. allow_interactive_reauth=false: this auth provider is
	// stored in GlobalFunctionData and its GetAuthorizationHeader is called
	// again from GSheetWriteSink on every batch, potentially from a different
	// worker thread each time under a parallel COPY (PER_THREAD_OUTPUT) - see
	// CreateAuthFromSecret's doc. An invalid_grant there still fails cleanly,
	// same as before automatic reauth existed; only a single-threaded caller
	// like ReadSheetBind gets the interactive reauth path.
	auto http = sheets::CreateHttpClient(context);
	auto auth = sheets::CreateAuthFromSecret(context, *http, /*allow_interactive_reauth=*/false);
	if (!auth) {
		throw InvalidInputException("No 'gsheet' secret found...");
	}
	auto client = sheets::GoogleSheetsClient(*http, *auth);

	// Prefer a sheet or range that is specified as a parameter over one on the query string
	if (!options.sheet.empty()) {
		sheet_name = options.sheet;
	} else if (!options.gid.empty()) {
		try {
			auto sheet = client.Spreadsheets(spreadsheet_id).GetSheetById(options.gid);
			sheet_name = sheet.properties.title;
		} catch (const std::invalid_argument &e) {
			throw InvalidInputException("Cannot convert sheet ID " + options.gid + " to integer: " + e.what());
		} catch (const std::out_of_range &e) {
			throw InvalidInputException("Cannot convert sheet ID " + options.gid + " to integer: " + e.what());
		}
	} else {
		// Fallback to first sheet by index, same as read_gsheet
		auto sheet = client.Spreadsheets(spreadsheet_id).GetSheetByIndex(0);
		sheet_name = sheet.properties.title;
	}

	// Create sheet if not exist (if enabled)
	if (options.create_if_not_exists) {
		try {
			auto sheet = client.Spreadsheets(spreadsheet_id).GetSheetByName(sheet_name);
			// sheet already exists so we need to ignore this instruction from the user
		} catch (sheets::SheetNotFoundException &_) {
			client.Spreadsheets(spreadsheet_id).CreateSheet(sheet_name);
		}
	}

	std::string encoded_sheet_name = url_encode(sheet_name);

	// Do this here in the initialization so that it only happens once
	// OVERWRITE_RANGE takes precedence since it defaults to false and is less destructive
	if (options.overwrite_range) {
		client.Spreadsheets(spreadsheet_id).Values().Clear(sheets::A1Range(encoded_sheet_name + "!" + sheet_range));
	} else if (options.overwrite_sheet) {
		client.Spreadsheets(spreadsheet_id).Values().Clear(sheets::A1Range(encoded_sheet_name));
	}

	// Make the API call to write headers to the Google Sheet
	// If we are appending, header defaults to false
	if (options.header) {
		sheets::ValueRange header_values;
		header_values.range = sheet_range.empty() ? sheet_name : sheet_name + "!" + sheet_range;
		header_values.majorDimension = sheets::ROWS;
		header_values.values.push_back(options.name_list);

		auto range_str = encoded_sheet_name;
		if (!sheet_range.empty()) {
			range_str += "!" + sheet_range;
		}
		client.Spreadsheets(spreadsheet_id).Values().Append(sheets::A1Range(range_str), header_values);
	}
	return make_uniq<GSheetCopyGlobalState>(context, std::move(http), std::move(auth), spreadsheet_id,
	                                        sheet_name);
}

unique_ptr<LocalFunctionData> GSheetCopyFunction::GSheetWriteInitializeLocal(ExecutionContext &context,
                                                                             FunctionData &bind_data_p) {
	return make_uniq<LocalFunctionData>();
}

void GSheetCopyFunction::GSheetWriteCombine(ExecutionContext &context, FunctionData &bind_data,
                                            GlobalFunctionData &gstate, LocalFunctionData &lstate) {
}

void GSheetCopyFunction::GSheetWriteFinalize(ClientContext &context, FunctionData &bind_data,
                                             GlobalFunctionData &gstate) {
}

void GSheetCopyFunction::GSheetWriteSink(ExecutionContext &context, FunctionData &bind_data_p,
                                         GlobalFunctionData &gstate_p, LocalFunctionData &lstate, DataChunk &input) {
	input.Flatten();
	auto &gstate = gstate_p.Cast<GSheetCopyGlobalState>();

	auto options = bind_data_p.Cast<GSheetWriteBindData>().options;
	// Resolved once in GSheetWriteInitializeGlobal
	const std::string &sheet_name = gstate.sheet_name;
	std::string sheet_range = options.range;

	auto client = sheets::GoogleSheetsClient(*gstate.http, *gstate.auth);

	std::string encoded_sheet_name = url_encode(sheet_name);

	// Build ValueRange from input chunk
	sheets::ValueRange data;
	data.range = sheet_range.empty() ? sheet_name : sheet_name + "!" + sheet_range;
	data.majorDimension = sheets::ROWS;

	for (idx_t row_ix = 0; row_ix < input.size(); row_ix++) {
		std::vector<std::string> row;
		for (idx_t col_ix = 0; col_ix < input.ColumnCount(); col_ix++) {
			auto &col = input.data[col_ix];
			Value val = col.GetValue(row_ix);
			if (val.IsNull()) {
				row.emplace_back("");
			} else {
				row.emplace_back(val.ToString());
			}
		}
		data.values.emplace_back(std::move(row));
	}
	auto range_str = encoded_sheet_name;
	if (!sheet_range.empty()) {
		range_str += "!" + sheet_range;
	}
	client.Spreadsheets(gstate.spreadsheet_id).Values().Append(sheets::A1Range(range_str), data);
}
} // namespace duckdb
