#include "duckdb.hpp"

#include <libpq-fe.h>

#include "duckdb/main/extension_util.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "postgres_scanner.hpp"
#include "postgres_result.hpp"
#include "postgres_conversion.hpp"

namespace duckdb {

static constexpr uint32_t POSTGRES_TID_MAX = 4294967295;

struct PostgresLocalState : public LocalTableFunctionState {
	~PostgresLocalState() {
		if (conn) {
			PQfinish(conn);
			conn = nullptr;
		}
	}

	bool done = false;
	bool exec = false;
	string sql;
	vector<column_t> column_ids;
	TableFilterSet *filters;
	string col_names;
	PGconn *conn = nullptr;
};

struct PostgresGlobalState : public GlobalTableFunctionState {
	PostgresGlobalState(idx_t max_threads) : page_idx(0), max_threads(max_threads) {
	}

	mutex lock;
	idx_t page_idx;
	idx_t max_threads;

	idx_t MaxThreads() const override {
		return max_threads;
	}
};

static unique_ptr<PostgresResult> PGQuery(PGconn *conn, string q, ExecStatusType response_code = PGRES_TUPLES_OK) {
	auto res = make_uniq<PostgresResult>(PQexec(conn, q.c_str()));
	if (!res->res || PQresultStatus(res->res) != response_code) {
		throw IOException("Unable to query Postgres: %s %s", string(PQerrorMessage(conn)),
		                  string(PQresultErrorMessage(res->res)));
	}
	return res;
}

static void PGExec(PGconn *conn, string q) {
	PGQuery(conn, q, PGRES_COMMAND_OK);
}

static LogicalType DuckDBType2(PostgresTypeInfo *type_info, int atttypmod, PostgresTypeInfo *ele_info, PGconn *conn,
                               ClientContext &context) {
	auto &pgtypename = type_info->typname;

	// TODO better check, does the typtyp say something here?
	// postgres array types start with an _
	if (StringUtil::StartsWith(pgtypename, "_")) {
		return LogicalType::LIST(DuckDBType2(ele_info, atttypmod, nullptr, conn, context));
	}

	if (type_info->typtype == "e") { // ENUM
		auto res = PGQuery(
		    conn, StringUtil::Format("SELECT unnest(enum_range(NULL::%s.%s))", type_info->nspname, type_info->typname));
		Vector duckdb_levels(LogicalType::VARCHAR, res->Count());
		for (idx_t row = 0; row < res->Count(); row++) {
			duckdb_levels.SetValue(row, res->GetString(row, 0));
		}
		return LogicalType::ENUM("postgres_enum_" + pgtypename, duckdb_levels, res->Count());
	}

	if (pgtypename == "bool") {
		return LogicalType::BOOLEAN;
	} else if (pgtypename == "int2") {
		return LogicalType::SMALLINT;
	} else if (pgtypename == "int4") {
		return LogicalType::INTEGER;
	} else if (pgtypename == "int8") {
		return LogicalType::BIGINT;
	} else if (pgtypename == "oid") { // "The oid type is currently implemented as an unsigned four-byte integer."
		return LogicalType::UINTEGER;
	} else if (pgtypename == "float4") {
		return LogicalType::FLOAT;
	} else if (pgtypename == "float8") {
		return LogicalType::DOUBLE;
	} else if (pgtypename == "numeric") {
		if (atttypmod == -1) { // unbounded decimal/numeric, will just return as double
			return LogicalType::DOUBLE;
		}
		auto width = ((atttypmod - sizeof(int32_t)) >> 16) & 0xffff;
		auto scale = (((atttypmod - sizeof(int32_t)) & 0x7ff) ^ 1024) - 1024;
		return LogicalType::DECIMAL(width, scale);
	} else if (pgtypename == "char" || pgtypename == "bpchar" || pgtypename == "varchar" || pgtypename == "text" ||
	           pgtypename == "jsonb" || pgtypename == "json") {
		return LogicalType::VARCHAR;
	} else if (pgtypename == "date") {
		return LogicalType::DATE;
	} else if (pgtypename == "bytea") {
		return LogicalType::BLOB;
	} else if (pgtypename == "time") {
		return LogicalType::TIME;
	} else if (pgtypename == "timetz") {
		return LogicalType::TIME_TZ;
	} else if (pgtypename == "timestamp") {
		return LogicalType::TIMESTAMP;
	} else if (pgtypename == "timestamptz") {
		return LogicalType::TIMESTAMP_TZ;
	} else if (pgtypename == "interval") {
		return LogicalType::INTERVAL;
	} else if (pgtypename == "uuid") {
		return LogicalType::UUID;
	} else {
		return LogicalType::INVALID;
	}
}

static LogicalType DuckDBType(PostgresColumnInfo &info, PGconn *conn, ClientContext &context) {
	return DuckDBType2(&info.type_info, info.atttypmod, &info.elem_info, conn, context);
}

void PostgresScanFunction::PrepareBind(ClientContext &context, PostgresBindData &bind_data) {
	// we create a transaction here, and get the snapshot id so the parallel
	// reader threads can use the same snapshot
	bind_data.in_recovery = (bool)PGQuery(bind_data.conn, "SELECT pg_is_in_recovery()")->GetBool(0, 0);
	bind_data.snapshot = "";

	if (!bind_data.in_recovery) {
		bind_data.snapshot = PGQuery(bind_data.conn, "SELECT pg_export_snapshot()")->GetString(0, 0);
	}

	// find the id of the table in question to simplify below queries and avoid
	// complex joins (ha)
	auto res = PGQuery(bind_data.conn, StringUtil::Format(R"(
SELECT pg_class.oid, GREATEST(relpages, 1)
FROM pg_class JOIN pg_namespace ON relnamespace = pg_namespace.oid
WHERE nspname=%s AND relname=%s
)", KeywordHelper::WriteQuoted(bind_data.schema_name), KeywordHelper::WriteQuoted(bind_data.table_name)));
	if (res->Count() != 1) {
		throw InvalidInputException("Postgres table \"%s\".\"%s\" not found", bind_data.schema_name,
		                            bind_data.table_name);
	}
	auto oid = res->GetInt64(0, 0);
	bind_data.pages_approx = res->GetInt64(0, 1);

	res.reset();

	// query the table schema so we can interpret the bits in the pages
	// fun fact: this query also works in DuckDB ^^
	res = PGQuery(bind_data.conn, StringUtil::Format(
	                                   R"(
SELECT
    attname, atttypmod, pg_namespace.nspname,
    pg_type.typname, pg_type.typlen, pg_type.typtype, pg_type.typelem,
    pg_type_elem.typname elem_typname, pg_type_elem.typlen elem_typlen, pg_type_elem.typtype elem_typtype
FROM pg_attribute
    JOIN pg_type ON atttypid=pg_type.oid
    LEFT JOIN pg_type pg_type_elem ON pg_type.typelem=pg_type_elem.oid
    LEFT JOIN pg_namespace ON pg_type.typnamespace = pg_namespace.oid
WHERE attrelid=%d AND attnum > 0
ORDER BY attnum;
)",
	                                   oid));

	// can't scan a table without columns (yes those exist)
	if (res->Count() == 0) {
		throw InvalidInputException("Table %s does not contain any columns.", bind_data.table_name);
	}

	for (idx_t row = 0; row < res->Count(); row++) {
		PostgresColumnInfo info;
		info.attname = res->GetString(row, 0);
		info.atttypmod = res->GetInt32(row, 1);

		info.type_info.nspname = res->GetString(row, 2);
		info.type_info.typname = res->GetString(row, 3);
		info.type_info.typlen = res->GetInt64(row, 4);
		info.type_info.typtype = res->GetString(row, 5);
		info.typelem = res->GetInt64(row, 6);

		info.elem_info.nspname = res->GetString(row, 2);
		info.elem_info.typname = res->GetString(row, 7);
		info.elem_info.typlen = res->GetInt64(row, 8);
		info.elem_info.typtype = res->GetString(row, 9);

		bind_data.names.push_back(info.attname);
		auto duckdb_type = DuckDBType(info, bind_data.conn, context);
		// we cast unsupported types to varchar on read
		auto needs_cast = duckdb_type == LogicalType::INVALID;
		bind_data.needs_cast.push_back(needs_cast);
		if (!needs_cast) {
			bind_data.types.push_back(std::move(duckdb_type));
		} else {
			bind_data.types.push_back(LogicalType::VARCHAR);
		}

		bind_data.columns.push_back(info);
	}
	res.reset();
}

static unique_ptr<FunctionData> PostgresBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<PostgresBindData>();

	bind_data->dsn = input.inputs[0].GetValue<string>();
	bind_data->schema_name = input.inputs[1].GetValue<string>();
	bind_data->table_name = input.inputs[2].GetValue<string>();

	bind_data->conn = PostgresUtils::PGConnect(bind_data->dsn);

	PGExec(bind_data->conn, "BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ READ ONLY");
	PostgresScanFunction::PrepareBind(context, *bind_data);

	return_types = bind_data->types;
	names = bind_data->names;

	return std::move(bind_data);
}

static string TransformFilter(string &column_name, TableFilter &filter);

static string CreateExpression(string &column_name, vector<unique_ptr<TableFilter>> &filters, string op) {
	vector<string> filter_entries;
	for (auto &filter : filters) {
		filter_entries.push_back(TransformFilter(column_name, *filter));
	}
	return "(" + StringUtil::Join(filter_entries, " " + op + " ") + ")";
}

static string TransformComparision(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		return "=";
	case ExpressionType::COMPARE_NOTEQUAL:
		return "!=";
	case ExpressionType::COMPARE_LESSTHAN:
		return "<";
	case ExpressionType::COMPARE_GREATERTHAN:
		return ">";
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return "<=";
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ">=";
	default:
		throw NotImplementedException("Unsupported expression type");
	}
}

static string TransformFilter(string &column_name, TableFilter &filter) {
	switch (filter.filter_type) {
	case TableFilterType::IS_NULL:
		return column_name + " IS NULL";
	case TableFilterType::IS_NOT_NULL:
		return column_name + " IS NOT NULL";
	case TableFilterType::CONJUNCTION_AND: {
		auto &conjunction_filter = (ConjunctionAndFilter &)filter;
		return CreateExpression(column_name, conjunction_filter.child_filters, "AND");
	}
	case TableFilterType::CONJUNCTION_OR: {
		auto &conjunction_filter = (ConjunctionAndFilter &)filter;
		return CreateExpression(column_name, conjunction_filter.child_filters, "OR");
	}
	case TableFilterType::CONSTANT_COMPARISON: {
		auto &constant_filter = (ConstantFilter &)filter;
		// TODO properly escape ' in constant value
		auto constant_string = "'" + constant_filter.constant.ToString() + "'";
		auto operator_string = TransformComparision(constant_filter.comparison_type);
		return StringUtil::Format("%s %s %s", column_name, operator_string, constant_string);
	}
	default:
		throw InternalException("Unsupported table filter type");
	}
}

static void PostgresInitInternal(ClientContext &context, const PostgresBindData *bind_data_p,
                                 PostgresLocalState &lstate, idx_t task_min, idx_t task_max) {
	D_ASSERT(bind_data_p);
	D_ASSERT(task_min <= task_max);

	auto bind_data = (const PostgresBindData *)bind_data_p;

	string col_names;
	for(auto &column_id : lstate.column_ids) {
		if (!col_names.empty()) {
			col_names += ", ";
		}
		if (column_id == COLUMN_IDENTIFIER_ROW_ID) {
			col_names += "ctid";
		} else {
			col_names += KeywordHelper::WriteQuoted(bind_data->names[column_id], '"');
			if (bind_data->needs_cast[column_id]) {
				col_names += "::VARCHAR";
			}
		}
	}

	string filter_string;
	if (lstate.filters && !lstate.filters->filters.empty()) {
		vector<string> filter_entries;
		for (auto &entry : lstate.filters->filters) {
			auto column_name = KeywordHelper::WriteQuoted(bind_data->names[lstate.column_ids[entry.first]]);
			auto &filter = *entry.second;
			filter_entries.push_back(TransformFilter(column_name, filter));
		}
		filter_string = " AND " + StringUtil::Join(filter_entries, " AND ");
	}

	lstate.sql = StringUtil::Format(
	    R"(
COPY (SELECT %s FROM %s.%s WHERE ctid BETWEEN '(%d,0)'::tid AND '(%d,0)'::tid %s) TO STDOUT (FORMAT binary);
)",

	    col_names, KeywordHelper::WriteQuoted(bind_data->schema_name, '"'), KeywordHelper::WriteQuoted(bind_data->table_name, '"'), task_min, task_max, filter_string);

	lstate.exec = false;
	lstate.done = false;
}

static PGconn *PostgresScanConnect(string dsn, bool in_recovery, string snapshot) {
	auto conn = PostgresUtils::PGConnect(dsn);
	PGExec(conn, "BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ READ ONLY");
	if (!in_recovery) {
		PGExec(conn, StringUtil::Format("SET TRANSACTION SNAPSHOT '%s'", snapshot));
	}
	return conn;
}

#define NBASE      10000
#define DEC_DIGITS 4 /* decimal digits per NBASE digit */

/*
 * Interpretation of high bits.
 */

#define NUMERIC_SIGN_MASK 0xC000
#define NUMERIC_POS       0x0000
#define NUMERIC_NEG       0x4000
#define NUMERIC_SHORT     0x8000
#define NUMERIC_SPECIAL   0xC000

/*
 * Definitions for special values (NaN, positive infinity, negative infinity).
 *
 * The two bits after the NUMERIC_SPECIAL bits are 00 for NaN, 01 for positive
 * infinity, 11 for negative infinity.  (This makes the sign bit match where
 * it is in a short-format value, though we make no use of that at present.)
 * We could mask off the remaining bits before testing the active bits, but
 * currently those bits must be zeroes, so masking would just add cycles.
 */
#define NUMERIC_EXT_SIGN_MASK 0xF000 /* high bits plus NaN/Inf flag bits */
#define NUMERIC_NAN           0xC000
#define NUMERIC_PINF          0xD000
#define NUMERIC_NINF          0xF000
#define NUMERIC_INF_SIGN_MASK 0x2000

#define NUMERIC_EXT_FLAGBITS(n) ((n)->choice.n_header & NUMERIC_EXT_SIGN_MASK)
#define NUMERIC_IS_NAN(n)       ((n)->choice.n_header == NUMERIC_NAN)
#define NUMERIC_IS_PINF(n)      ((n)->choice.n_header == NUMERIC_PINF)
#define NUMERIC_IS_NINF(n)      ((n)->choice.n_header == NUMERIC_NINF)
#define NUMERIC_IS_INF(n)       (((n)->choice.n_header & ~NUMERIC_INF_SIGN_MASK) == NUMERIC_PINF)

/*
 * Short format definitions.
 */

#define NUMERIC_DSCALE_MASK            0x3FFF
#define NUMERIC_SHORT_SIGN_MASK        0x2000
#define NUMERIC_SHORT_DSCALE_MASK      0x1F80
#define NUMERIC_SHORT_DSCALE_SHIFT     7
#define NUMERIC_SHORT_DSCALE_MAX       (NUMERIC_SHORT_DSCALE_MASK >> NUMERIC_SHORT_DSCALE_SHIFT)
#define NUMERIC_SHORT_WEIGHT_SIGN_MASK 0x0040
#define NUMERIC_SHORT_WEIGHT_MASK      0x003F
#define NUMERIC_SHORT_WEIGHT_MAX       NUMERIC_SHORT_WEIGHT_MASK
#define NUMERIC_SHORT_WEIGHT_MIN       (-(NUMERIC_SHORT_WEIGHT_MASK + 1))

#define NUMERIC_SIGN(is_short, header1)                                                                                \
	(is_short ? ((header1 & NUMERIC_SHORT_SIGN_MASK) ? NUMERIC_NEG : NUMERIC_POS) : (header1 & NUMERIC_SIGN_MASK))
#define NUMERIC_DSCALE(is_short, header1)                                                                              \
	(is_short ? (header1 & NUMERIC_SHORT_DSCALE_MASK) >> NUMERIC_SHORT_DSCALE_SHIFT : (header1 & NUMERIC_DSCALE_MASK))
#define NUMERIC_WEIGHT(is_short, header1, header2)                                                                     \
	(is_short ? ((header1 & NUMERIC_SHORT_WEIGHT_SIGN_MASK ? ~NUMERIC_SHORT_WEIGHT_MASK : 0) |                         \
	             (header1 & NUMERIC_SHORT_WEIGHT_MASK))                                                                \
	          : (header2))

// copied from cast_helpers.cpp because windows linking issues
static const int64_t POWERS_OF_TEN[] {1,
                                      10,
                                      100,
                                      1000,
                                      10000,
                                      100000,
                                      1000000,
                                      10000000,
                                      100000000,
                                      1000000000,
                                      10000000000,
                                      100000000000,
                                      1000000000000,
                                      10000000000000,
                                      100000000000000,
                                      1000000000000000,
                                      10000000000000000,
                                      100000000000000000,
                                      1000000000000000000};

struct PostgresDecimalConfig {
	uint16_t scale;
	uint16_t ndigits;
	int16_t weight;
	bool is_negative;
};

static PostgresDecimalConfig ReadDecimalConfig(const_data_ptr_t &value_ptr) {
	PostgresDecimalConfig config;
	config.ndigits = PostgresConversion::LoadInteger<uint16_t>(value_ptr);
	config.weight = PostgresConversion::LoadInteger<int16_t>(value_ptr);
	auto sign = PostgresConversion::LoadInteger<uint16_t>(value_ptr);

	if (!(sign == NUMERIC_POS || sign == NUMERIC_NAN || sign == NUMERIC_PINF || sign == NUMERIC_NINF ||
	      sign == NUMERIC_NEG)) {
		throw NotImplementedException("Postgres numeric NA/Inf");
	}
	config.is_negative = sign == NUMERIC_NEG;
	config.scale = PostgresConversion::LoadInteger<uint16_t>(value_ptr);

	return config;
};

template <class T>
static T ReadDecimal(PostgresDecimalConfig &config, const_data_ptr_t value_ptr) {
	// this is wild
	auto scale_POWER = POWERS_OF_TEN[config.scale];

	if (config.ndigits == 0) {
		return 0;
	}
	T integral_part = 0, fractional_part = 0;

	if (config.weight >= 0) {
		D_ASSERT(config.weight <= config.ndigits);
		integral_part = PostgresConversion::LoadInteger<uint16_t>(value_ptr);
		for (auto i = 1; i <= config.weight; i++) {
			integral_part *= NBASE;
			if (i < config.ndigits) {
				integral_part += PostgresConversion::LoadInteger<uint16_t>(value_ptr);
			}
		}
		integral_part *= scale_POWER;
	}

	if (config.ndigits > config.weight + 1) {
		fractional_part = PostgresConversion::LoadInteger<uint16_t>(value_ptr);
		for (auto i = config.weight + 2; i < config.ndigits; i++) {
			fractional_part *= NBASE;
			if (i < config.ndigits) {
				fractional_part += PostgresConversion::LoadInteger<uint16_t>(value_ptr);
			}
		}

		// we need to find out how large the fractional part is in terms of powers
		// of ten this depends on how many times we multiplied with NBASE
		// if that is different from scale, we need to divide the extra part away
		// again
		// similarly, if trailing zeroes have been suppressed, we have not been multiplying t
		// the fractional part with NBASE often enough. If so, add additional powers
		auto fractional_power = (config.ndigits - config.weight - 1) * DEC_DIGITS;
		auto fractional_power_correction = fractional_power - config.scale;
		D_ASSERT(fractional_power_correction < 20);
		if (fractional_power_correction >= 0) {
			fractional_part /= POWERS_OF_TEN[fractional_power_correction];
		} else {
			fractional_part *= POWERS_OF_TEN[-fractional_power_correction];
		}
	}

	// finally
	auto base_res = (integral_part + fractional_part);
	return (config.is_negative ? -base_res : base_res);
}

static void ProcessValue(const LogicalType &type, const PostgresTypeInfo *type_info, int atttypmod, int64_t typelem,
                         const PostgresTypeInfo *elem_info, const_data_ptr_t value_ptr, idx_t value_len,
                         Vector &out_vec, idx_t output_offset) {

	switch (type.id()) {
	case LogicalTypeId::SMALLINT:
		D_ASSERT(value_len == sizeof(int16_t));
		FlatVector::GetData<int16_t>(out_vec)[output_offset] = PostgresConversion::LoadInteger<int16_t>(value_ptr);
		break;

	case LogicalTypeId::INTEGER:
		D_ASSERT(value_len == sizeof(int32_t));
		FlatVector::GetData<int32_t>(out_vec)[output_offset] = PostgresConversion::LoadInteger<int32_t>(value_ptr);
		break;

	case LogicalTypeId::UINTEGER:
		D_ASSERT(value_len == sizeof(uint32_t));
		FlatVector::GetData<uint32_t>(out_vec)[output_offset] = PostgresConversion::LoadInteger<uint32_t>(value_ptr);
		break;

	case LogicalTypeId::BIGINT:
		D_ASSERT(value_len == sizeof(int64_t));
		FlatVector::GetData<int64_t>(out_vec)[output_offset] = PostgresConversion::LoadInteger<int64_t>(value_ptr);
		break;

	case LogicalTypeId::FLOAT: {
		D_ASSERT(value_len == sizeof(float));
		FlatVector::GetData<float>(out_vec)[output_offset] = PostgresConversion::LoadFloat(value_ptr);
		break;
	}

	case LogicalTypeId::DOUBLE: {
		// this was an unbounded decimal, read params from value and cast to double
		if (type_info->typname == "numeric") {
			auto config = ReadDecimalConfig(value_ptr);
			auto val = ReadDecimal<int64_t>(config, value_ptr);
			FlatVector::GetData<double>(out_vec)[output_offset] = (double)val / POWERS_OF_TEN[config.scale];
			break;
		}
		D_ASSERT(value_len == sizeof(double));
		FlatVector::GetData<double>(out_vec)[output_offset] = PostgresConversion::LoadDouble(value_ptr);
		break;
	}

	case LogicalTypeId::BLOB:
	case LogicalTypeId::VARCHAR: {
		if (type_info->typname == "jsonb") {
			auto version = Load<uint8_t>(value_ptr);
			value_ptr++;
			value_len--;
			if (version != 1) {
				throw NotImplementedException("JSONB version number mismatch, expected 1, got %d", version);
			}
		}
		FlatVector::GetData<string_t>(out_vec)[output_offset] =
		    StringVector::AddStringOrBlob(out_vec, (char *)value_ptr, value_len);
		break;
	}
	case LogicalTypeId::BOOLEAN:
		D_ASSERT(value_len == sizeof(bool));
		FlatVector::GetData<bool>(out_vec)[output_offset] = *value_ptr > 0;
		break;
	case LogicalTypeId::DECIMAL: {
		if (value_len < sizeof(uint16_t) * 4) {
			throw InvalidInputException("Need at least 8 bytes to read a Postgres decimal. Got %d", value_len);
		}
		auto decimal_config = ReadDecimalConfig(value_ptr);
		D_ASSERT(decimal_config.scale == DecimalType::GetScale(type));

		switch (type.InternalType()) {
		case PhysicalType::INT16:
			FlatVector::GetData<int16_t>(out_vec)[output_offset] = ReadDecimal<int16_t>(decimal_config, value_ptr);
			break;
		case PhysicalType::INT32:
			FlatVector::GetData<int32_t>(out_vec)[output_offset] = ReadDecimal<int32_t>(decimal_config, value_ptr);
			break;
		case PhysicalType::INT64:
			FlatVector::GetData<int64_t>(out_vec)[output_offset] = ReadDecimal<int64_t>(decimal_config, value_ptr);
			break;
		case PhysicalType::INT128:
			FlatVector::GetData<hugeint_t>(out_vec)[output_offset] = ReadDecimal<hugeint_t>(decimal_config, value_ptr);
			break;
		default:
			throw InvalidInputException("Unsupported decimal storage type");
		}
		break;
	}

	case LogicalTypeId::DATE: {
		D_ASSERT(value_len == sizeof(int32_t));
		auto out_ptr = FlatVector::GetData<date_t>(out_vec);
		out_ptr[output_offset] = PostgresConversion::LoadDate(value_ptr);
		break;
	}

	case LogicalTypeId::TIME: {
		D_ASSERT(value_len == sizeof(int64_t));
		D_ASSERT(atttypmod == -1);

		FlatVector::GetData<dtime_t>(out_vec)[output_offset].micros = PostgresConversion::LoadInteger<uint64_t>(value_ptr);
		break;
	}

	case LogicalTypeId::TIME_TZ: {
		D_ASSERT(value_len == sizeof(int64_t) + sizeof(int32_t));
		D_ASSERT(atttypmod == -1);

		auto usec = PostgresConversion::LoadInteger<uint64_t>(value_ptr);
		auto tzoffset = PostgresConversion::LoadInteger<int32_t>(value_ptr);
		FlatVector::GetData<dtime_t>(out_vec)[output_offset].micros = usec + tzoffset * Interval::MICROS_PER_SEC;
		break;
	}

	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP: {
		D_ASSERT(value_len == sizeof(int64_t));
		D_ASSERT(atttypmod == -1);
		FlatVector::GetData<timestamp_t>(out_vec)[output_offset] = PostgresConversion::LoadTimestamp(value_ptr);
		break;
	}
	case LogicalTypeId::ENUM: {
		auto enum_val = string((const char *)value_ptr, value_len);
		auto offset = EnumType::GetPos(type, enum_val);
		if (offset < 0) {
			throw IOException("Could not map ENUM value %s", enum_val);
		}
		switch (type.InternalType()) {
		case PhysicalType::UINT8:
			FlatVector::GetData<uint8_t>(out_vec)[output_offset] = (uint8_t)offset;
			break;
		case PhysicalType::UINT16:
			FlatVector::GetData<uint16_t>(out_vec)[output_offset] = (uint16_t)offset;
			break;

		case PhysicalType::UINT32:
			FlatVector::GetData<uint32_t>(out_vec)[output_offset] = (uint32_t)offset;
			break;

		default:
			throw InternalException("ENUM can only have unsigned integers (except "
			                        "UINT64) as physical types, got %s",
			                        TypeIdToString(type.InternalType()));
		}
		break;
	}
	case LogicalTypeId::INTERVAL: {
		if (atttypmod != -1) {
			throw IOException("Interval with unsupported typmod %d", atttypmod);
		}
		FlatVector::GetData<interval_t>(out_vec)[output_offset] = PostgresConversion::LoadInterval(value_ptr);
		break;
	}

	case LogicalTypeId::UUID: {
		D_ASSERT(value_len == 2 * sizeof(int64_t));
		D_ASSERT(atttypmod == -1);
		FlatVector::GetData<hugeint_t>(out_vec)[output_offset] = PostgresConversion::LoadUUID(value_ptr);
		break;
	}

	case LogicalTypeId::LIST: {
		D_ASSERT(elem_info);
		auto &list_entry = FlatVector::GetData<list_entry_t>(out_vec)[output_offset];
		auto child_offset = ListVector::GetListSize(out_vec);

		if (value_len < 1) {
			list_entry.offset = ListVector::GetListSize(out_vec);
			list_entry.length = 0;
			break;
		}
		D_ASSERT(value_len >= 3 * sizeof(uint32_t));
		auto flag_one = PostgresConversion::LoadInteger<uint32_t>(value_ptr);
		auto flag_two = PostgresConversion::LoadInteger<uint32_t>(value_ptr);
		if (flag_one == 0) {
			list_entry.offset = child_offset;
			list_entry.length = 0;
			return;
		}
		// D_ASSERT(flag_two == 1); // TODO what is this?!
		auto value_oid = PostgresConversion::LoadInteger<uint32_t>(value_ptr);
		D_ASSERT(value_oid == typelem);
		auto array_length = PostgresConversion::LoadInteger<uint32_t>(value_ptr);
		auto array_dim = PostgresConversion::LoadInteger<uint32_t>(value_ptr);
		if (array_dim != 1) {
			throw NotImplementedException("Only one-dimensional Postgres arrays are supported %u %u ", array_length,
			                              array_dim);
		}

		auto &child_vec = ListVector::GetEntry(out_vec);
		ListVector::Reserve(out_vec, child_offset + array_length);
		for (idx_t child_idx = 0; child_idx < array_length; child_idx++) {
			// handle NULLs again (TODO: unify this with scan)
			auto ele_len = PostgresConversion::LoadInteger<int32_t>(value_ptr);
			if (ele_len == -1) { // NULL
				FlatVector::Validity(child_vec).Set(child_offset + child_idx, false);
				continue;
			}

			ProcessValue(ListType::GetChildType(type), elem_info, atttypmod, 0, nullptr, value_ptr, ele_len, child_vec,
			             child_offset + child_idx);
			value_ptr += ele_len;
		}
		ListVector::SetListSize(out_vec, child_offset + array_length);

		list_entry.offset = child_offset;
		list_entry.length = array_length;
		break;
	}

	default:
		throw InternalException("Unsupported Type %s", type.ToString());
	}
}

struct PostgresBinaryBuffer {

	PostgresBinaryBuffer(PGconn *conn_p) : conn(conn_p) {
		D_ASSERT(conn);
	}
	void Next() {
		Reset();
		len = PQgetCopyData(conn, &buffer, 0);

		// len -2 is error
		// len -1 is supposed to signal end but does not actually happen in practise
		// we expect at least 2 bytes in each message for the tuple count
		if (!buffer || len < sizeof(int16_t)) {
			throw IOException("Unable to read binary COPY data from Postgres: %s", string(PQerrorMessage(conn)));
		}
		buffer_ptr = buffer;
	}
	void Reset() {
		if (buffer) {
			PQfreemem(buffer);
		}
		buffer = nullptr;
		buffer_ptr = nullptr;
		len = 0;
	}
	bool Ready() {
		return buffer_ptr != nullptr;
	}
	~PostgresBinaryBuffer() {
		Reset();
	}

	void CheckHeader() {
		auto magic_len = 11;
		auto flags_len = 8;
		auto header_len = magic_len + flags_len;

		if (len < header_len) {
			throw IOException("Unable to read binary COPY data from Postgres, invalid header");
		}
		if (!memcmp(buffer_ptr, "PGCOPY\\n\\377\\r\\n\\0", magic_len)) {
			throw IOException("Expected Postgres binary COPY header, got something else");
		}
		buffer_ptr += header_len;
		// as far as i can tell the "Flags field" and the "Header
		// extension area length" do not contain anything interesting
	}

	template <typename T>
	const T Read() {
		T ret;
		D_ASSERT(len > 0);
		D_ASSERT(buffer);
		D_ASSERT(buffer_ptr);
		memcpy(&ret, buffer_ptr, sizeof(ret));
		buffer_ptr += sizeof(T);
		return ret;
	}

	char *buffer = nullptr, *buffer_ptr = nullptr;
	int len = 0;
	PGconn *conn = nullptr;
};

static idx_t PostgresMaxThreads(ClientContext &context, const FunctionData *bind_data_p) {
	D_ASSERT(bind_data_p);

	auto bind_data = (const PostgresBindData *)bind_data_p;
	return bind_data->pages_approx / bind_data->pages_per_task;
}

static unique_ptr<GlobalTableFunctionState> PostgresInitGlobalState(ClientContext &context,
                                                                    TableFunctionInitInput &input) {
	return make_uniq<PostgresGlobalState>(PostgresMaxThreads(context, input.bind_data.get()));
}

static bool PostgresParallelStateNext(ClientContext &context, const FunctionData *bind_data_p,
                                      PostgresLocalState &lstate, PostgresGlobalState &gstate) {
	D_ASSERT(bind_data_p);
	auto bind_data = (const PostgresBindData *)bind_data_p;

	lock_guard<mutex> parallel_lock(gstate.lock);

	if (gstate.page_idx < bind_data->pages_approx) {
		auto page_max = gstate.page_idx + bind_data->pages_per_task;
		if (page_max >= bind_data->pages_approx) {
			// the relpages entry is not the real max, so make the last task bigger
			page_max = POSTGRES_TID_MAX;
		}

		PostgresInitInternal(context, bind_data, lstate, gstate.page_idx, page_max);
		gstate.page_idx += bind_data->pages_per_task;
		return true;
	}
	lstate.done = true;
	return false;
}

static unique_ptr<LocalTableFunctionState> PostgresInitLocalState(ExecutionContext &context,
                                                                  TableFunctionInitInput &input,
                                                                  GlobalTableFunctionState *global_state) {
	auto &bind_data = input.bind_data->Cast<PostgresBindData>();
	auto &gstate = global_state->Cast<PostgresGlobalState>();

	auto local_state = make_uniq<PostgresLocalState>();
	local_state->column_ids = input.column_ids;
	local_state->conn = PostgresScanConnect(bind_data.dsn, bind_data.in_recovery, bind_data.snapshot);
	local_state->filters = input.filters.get();
	if (!PostgresParallelStateNext(context.client, input.bind_data.get(), *local_state, gstate)) {
		local_state->done = true;
	}
	return std::move(local_state);
}

static void PostgresScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<PostgresBindData>();
	auto &local_state = data.local_state->Cast<PostgresLocalState>();
	auto &gstate = data.global_state->Cast<PostgresGlobalState>();

	idx_t output_offset = 0;
	PostgresBinaryBuffer buf(local_state.conn);

	while (true) {
		if (local_state.done && !PostgresParallelStateNext(context, data.bind_data.get(), local_state, gstate)) {
			return;
		}

		if (!local_state.exec) {
			PGQuery(local_state.conn, local_state.sql, PGRES_COPY_OUT);
			local_state.exec = true;
			buf.Next();
			buf.CheckHeader();
			// the first tuple immediately follows the header in the first message, so
			// we have to keep the buffer alive for now.
		}

		output.SetCardinality(output_offset);
		if (output_offset == STANDARD_VECTOR_SIZE) {
			return;
		}

		if (!buf.Ready()) {
			buf.Next();
		}

		auto tuple_count = (int16_t)ntohs(buf.Read<uint16_t>());
		if (tuple_count == -1) { // done here, lets try to get more
			local_state.done = true;
			continue;
		}

		D_ASSERT(tuple_count == local_state.column_ids.size());

		for (idx_t output_idx = 0; output_idx < output.ColumnCount(); output_idx++) {
			auto col_idx = local_state.column_ids[output_idx];
			auto &out_vec = output.data[output_idx];
			auto raw_len = (int32_t)ntohl(buf.Read<uint32_t>());
			if (raw_len == -1) { // NULL
				FlatVector::Validity(out_vec).Set(output_offset, false);
				continue;
			}
			if (col_idx == COLUMN_IDENTIFIER_ROW_ID) {
				// row id
				// ctid in postgres are a composite type of (page_index, tuple_in_page)
				// the page index is a 4-byte integer, the tuple_in_page a 2-byte integer
				D_ASSERT(raw_len == 6);
				auto value_ptr = (const_data_ptr_t)buf.buffer_ptr;
				int64_t page_index = PostgresConversion::LoadInteger<int32_t>(value_ptr);
				int64_t row_in_page = PostgresConversion::LoadInteger<int16_t>(value_ptr);
				FlatVector::GetData<int64_t>(out_vec)[output_offset] = (page_index << 16LL) + row_in_page;
			} else {
				ProcessValue(bind_data.types[col_idx], &bind_data.columns[col_idx].type_info,
							 bind_data.columns[col_idx].atttypmod, bind_data.columns[col_idx].typelem,
							 &bind_data.columns[col_idx].elem_info, (data_ptr_t)buf.buffer_ptr, raw_len, out_vec,
							 output_offset);
			}
			buf.buffer_ptr += raw_len;
		}

		buf.Reset();
		output_offset++;
	}
}

static string PostgresScanToString(const FunctionData *bind_data_p) {
	D_ASSERT(bind_data_p);

	auto bind_data = (const PostgresBindData *)bind_data_p;
	return bind_data->table_name;
}

struct AttachFunctionData : public TableFunctionData {
	AttachFunctionData() {
	}

	bool finished = false;
	string source_schema = "public";
	string sink_schema = "main";
	string suffix = "";
	bool overwrite = false;
	bool filter_pushdown = false;

	string dsn = "";
};

static unique_ptr<FunctionData> AttachBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {

	auto result = make_uniq<AttachFunctionData>();
	result->dsn = input.inputs[0].GetValue<string>();

	for (auto &kv : input.named_parameters) {
		if (kv.first == "source_schema") {
			result->source_schema = StringValue::Get(kv.second);
		} else if (kv.first == "sink_schema") {
			result->sink_schema = StringValue::Get(kv.second);
		} else if (kv.first == "overwrite") {
			result->overwrite = BooleanValue::Get(kv.second);
		} else if (kv.first == "filter_pushdown") {
			result->filter_pushdown = BooleanValue::Get(kv.second);
		}
	}

	return_types.push_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");
	return std::move(result);
}

static void AttachFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = (AttachFunctionData &)*data_p.bind_data;
	if (data.finished) {
		return;
	}

	auto conn = PostgresUtils::PGConnect(data.dsn);
	auto dconn = Connection(context.db->GetDatabase(context));
	auto res = PGQuery(conn, StringUtil::Format(
	                             R"(
SELECT relname
FROM pg_class JOIN pg_namespace ON pg_class.relnamespace = pg_namespace.oid
JOIN pg_attribute ON pg_class.oid = pg_attribute.attrelid
WHERE relkind = 'r' AND attnum > 0 AND nspname = '%s'
GROUP BY relname
ORDER BY relname;
)",
	                             data.source_schema)
	                             .c_str());

	for (idx_t row = 0; row < PQntuples(res->res); row++) {
		auto table_name = res->GetString(row, 0);

		dconn
		    .TableFunction(data.filter_pushdown ? "postgres_scan_pushdown" : "postgres_scan",
		                   {Value(data.dsn), Value(data.source_schema), Value(table_name)})
		    ->CreateView(data.sink_schema, table_name, data.overwrite, false);
	}
	res.reset();
	PQfinish(conn);

	data.finished = true;
}

PostgresScanFunction::PostgresScanFunction()
	: TableFunction("postgres_scan", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
					PostgresScan, PostgresBind, PostgresInitGlobalState, PostgresInitLocalState) {
	to_string = PostgresScanToString;
	projection_pushdown = true;
}

PostgresScanFunctionFilterPushdown::PostgresScanFunctionFilterPushdown()
	: TableFunction("postgres_scan_pushdown", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
					PostgresScan, PostgresBind, PostgresInitGlobalState, PostgresInitLocalState) {
	to_string = PostgresScanToString;
	projection_pushdown = true;
	filter_pushdown = true;
}

PostgresAttachFunction::PostgresAttachFunction()
	: TableFunction("postgres_attach", {LogicalType::VARCHAR}, AttachFunction, AttachBind) {
}

}
