//===----------------------------------------------------------------------===//
//                         DuckDB
//
// postgres_connection.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "postgres_utils.hpp"
#include "postgres_result.hpp"

namespace duckdb {
class PostgresStatement;
class PostgresResult;
struct IndexInfo;

class PostgresConnection {
public:
	PostgresConnection();
	PostgresConnection(PGconn *connection);
	~PostgresConnection();
	// disable copy constructors
	PostgresConnection(const PostgresConnection &other) = delete;
	PostgresConnection &operator=(const PostgresConnection &) = delete;
	//! enable move constructors
	PostgresConnection(PostgresConnection &&other) noexcept;
	PostgresConnection &operator=(PostgresConnection &&) noexcept;


public:
	static PostgresConnection Open(const string &connection_string);
	bool TryPrepare(const string &query, PostgresStatement &result, string &error);
	PostgresStatement Prepare(const string &query);
	void Execute(const string &query);
	unique_ptr<PostgresResult> TryQuery(const string &query);
	unique_ptr<PostgresResult> Query(const string &query);
	vector<string> GetTables();

	vector<string> GetEntries(string entry_type);
	bool GetTableInfo(const string &table_name, ColumnList &columns, vector<unique_ptr<Constraint>> &constraints);
	void GetViewInfo(const string &view_name, string &sql);
	void GetIndexInfo(const string &index_name, string &sql, string &table_name);
	//! Gets the max row id of a table, returns false if the table does not have a rowid column
	bool GetMaxRowId(const string &table_name, idx_t &row_id);
	bool ColumnExists(const string &table_name, const string &column_name);
	vector<IndexInfo> GetIndexInfo(const string &table_name);

	void BeginCopyTo(const string &table_name, const vector<string> &column_names);
	void CopyData(data_ptr_t buffer, idx_t size);
	void FinishCopyTo();

	bool IsOpen();
	void Close();

private:
	PGconn *connection;

};

} // namespace duckdb