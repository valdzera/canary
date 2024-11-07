/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-2024 OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "database/database.hpp"

#include "config/configmanager.hpp"
#include "lib/di/container.hpp"
#include "lib/metrics/metrics.hpp"
#include "utils/tools.hpp"

Database::~Database() {
	if (handle != nullptr) {
		mysql_close(handle);
	}
}

Database &Database::getInstance() {
	return inject<Database>();
}

bool Database::connect() {
	return connect(&g_configManager().getString(MYSQL_HOST), &g_configManager().getString(MYSQL_USER), &g_configManager().getString(MYSQL_PASS), &g_configManager().getString(MYSQL_DB), g_configManager().getNumber(SQL_PORT), &g_configManager().getString(MYSQL_SOCK));
}

bool Database::connect(const std::string* host, const std::string* user, const std::string* password, const std::string* database, uint32_t port, const std::string* sock) {
	// connection handle initialization
	handle = mysql_init(nullptr);
	if (!handle) {
		g_logger().error("Failed to initialize MySQL connection handle.");
		return false;
	}

	if (host->empty() || user->empty() || password->empty() || database->empty() || port <= 0) {
		g_logger().warn("MySQL host, user, password, database or port not provided");
	}

	// automatic reconnect
	bool reconnect = true;
	mysql_options(handle, MYSQL_OPT_RECONNECT, &reconnect);

	// Remove ssl verification
	bool ssl_enabled = false;
	mysql_options(handle, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &ssl_enabled);

	// connects to database
	if (!mysql_real_connect(handle, host->c_str(), user->c_str(), password->c_str(), database->c_str(), port, sock->c_str(), 0)) {
		g_logger().error("MySQL Error Message: {}", mysql_error(handle));
		return false;
	}

	DBResult_ptr result = storeQuery("SHOW VARIABLES LIKE 'max_allowed_packet'");
	if (result) {
		maxPacketSize = result->getNumber<uint64_t>("Value");
	}
	return true;
}

namespace {
	std::string createBackupFileName(const std::string &backupDir, const std::string &formattedTime) {
		std::filesystem::create_directories(backupDir);
		return fmt::format("{}/backup_{}.sql", backupDir, formattedTime);
	}

	MYSQL_RES* getTableList(MYSQL* handle) {
		if (mysql_query(handle, "SHOW TABLES") != 0) {
			g_logger().error("Failed to retrieve table list: {}", mysql_error(handle));
			return nullptr;
		}
		return mysql_store_result(handle);
	}

	void writeCreateTableStatement(MYSQL* handle, const std::string &tableName, std::ofstream &backupFile) {
		std::string createTableQuery = fmt::format("SHOW CREATE TABLE `{}`", tableName);
		if (mysql_query(handle, createTableQuery.c_str()) == 0) {
			MYSQL_RES* createTableResult = mysql_store_result(handle);
			if (createTableResult) {
				MYSQL_ROW createRow = mysql_fetch_row(createTableResult);
				if (createRow && createRow[1]) {
					backupFile << createRow[1] << ";\n\n";
				}
				mysql_free_result(createTableResult);
			}
		} else {
			g_logger().error("Failed to retrieve create statement for table {}: {}", tableName, mysql_error(handle));
		}
	}

	void writeValue(std::ofstream &backupFile, const char* value, const MYSQL_FIELD &field, unsigned long length) {
		if (!value) {
			backupFile << "NULL";
			return;
		}

		if (IS_BLOB(field.type)) {
			backupFile << g_database().escapeBlob(value, length);
		} else if (IS_NUM(field.type)) {
			backupFile << value;
		} else {
			backupFile << g_database().escapeString(std::string(value, length));
		}
	}

	void writeTableData(MYSQL* handle, const std::string &tableName, std::ofstream &backupFile) {
		std::string selectQuery = fmt::format("SELECT * FROM `{}`", tableName);
		if (mysql_query(handle, selectQuery.c_str()) != 0) {
			g_logger().error("Failed to retrieve data from table {}: {}", tableName, mysql_error(handle));
			return;
		}

		MYSQL_RES* tableData = mysql_store_result(handle);
		if (!tableData) {
			g_logger().error("Failed to store data result for table {}: {}", tableName, mysql_error(handle));
			return;
		}

		int numFields = mysql_num_fields(tableData);
		MYSQL_FIELD* fields = mysql_fetch_fields(tableData);
		MYSQL_ROW rowData;

		while ((rowData = mysql_fetch_row(tableData))) {
			auto lengths = mysql_fetch_lengths(tableData);
			backupFile << "INSERT INTO " << tableName << " VALUES(";

			for (int i = 0; i < numFields; ++i) {
				if (i > 0) {
					backupFile << ", ";
				}

				writeValue(backupFile, rowData[i], fields[i], lengths[i]);
			}

			backupFile << ");\n";
		}

		backupFile << "\n";
		mysql_free_result(tableData);
	}
}

void Database::createDatabaseBackup() const {
	if (!g_configManager().getBoolean(MYSQL_DB_BACKUP)) {
		return;
	}

	std::time_t now = getTimeNow();
	std::string formattedTime = fmt::format("{:%Y-%m-%d_%H-%M-%S}", fmt::localtime(now));

	if (formattedTime.empty()) {
		g_logger().error("Failed to format time for database backup.");
		return;
	}

	// Criar arquivo de backup
	std::string backupDir = "database_backup/";
	std::string backupFileName = createBackupFileName(backupDir, formattedTime);
	std::ofstream backupFile(backupFileName, std::ios::binary);

	if (!backupFile.is_open()) {
		g_logger().error("Failed to open backup file: {}", backupFileName);
		return;
	}

	if (!handle) {
		g_logger().error("Database handle not initialized.");
		return;
	}

	// Obter a lista de tabelas
	MYSQL_RES* tablesResult = getTableList(handle);
	if (!tablesResult) {
		return;
	}

	g_logger().info("Creating database backup...");
	MYSQL_ROW tableRow;

	// Iterar sobre as tabelas e gravar suas informações no arquivo de backup
	while ((tableRow = mysql_fetch_row(tablesResult))) {
		std::string tableName = tableRow[0];
		writeCreateTableStatement(handle, tableName, backupFile);
		writeTableData(handle, tableName, backupFile);
	}

	mysql_free_result(tablesResult);
	backupFile.close();

	g_logger().info("Database backup successfully created at: {}", backupFileName);
}

bool Database::beginTransaction() {
	if (!executeQuery("BEGIN")) {
		return false;
	}
	metrics::lock_latency measureLock("database");
	databaseLock.lock();
	measureLock.stop();

	return true;
}

bool Database::rollback() {
	if (!handle) {
		g_logger().error("Database not initialized!");
		return false;
	}

	if (mysql_rollback(handle) != 0) {
		g_logger().error("Message: {}", mysql_error(handle));
		databaseLock.unlock();
		return false;
	}

	databaseLock.unlock();
	return true;
}

bool Database::commit() {
	if (!handle) {
		g_logger().error("Database not initialized!");
		return false;
	}
	if (mysql_commit(handle) != 0) {
		g_logger().error("Message: {}", mysql_error(handle));
		databaseLock.unlock();
		return false;
	}

	databaseLock.unlock();
	return true;
}

bool Database::isRecoverableError(unsigned int error) {
	return error == CR_SERVER_LOST || error == CR_SERVER_GONE_ERROR || error == CR_CONN_HOST_ERROR || error == 1053 /*ER_SERVER_SHUTDOWN*/ || error == CR_CONNECTION_ERROR;
}

bool Database::retryQuery(std::string_view query, int retries) {
	while (retries > 0 && mysql_query(handle, query.data()) != 0) {
		g_logger().error("Query: {}", query.substr(0, 256));
		g_logger().error("MySQL error [{}]: {}", mysql_errno(handle), mysql_error(handle));
		if (!isRecoverableError(mysql_errno(handle))) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
		retries--;
	}
	if (retries == 0) {
		g_logger().error("Query {} failed after {} retries.", query, 10);
		return false;
	}

	return true;
}

bool Database::executeQuery(std::string_view query) {
	if (!handle) {
		g_logger().error("Database not initialized!");
		return false;
	}

	g_logger().trace("Executing Query: {}", query);

	metrics::lock_latency measureLock("database");
	std::scoped_lock lock { databaseLock };
	measureLock.stop();

	metrics::query_latency measure(query.substr(0, 50));
	bool success = retryQuery(query, 10);
	mysql_free_result(mysql_store_result(handle));

	return success;
}

DBResult_ptr Database::storeQuery(std::string_view query) {
	if (!handle) {
		g_logger().error("Database not initialized!");
		return nullptr;
	}
	g_logger().trace("Storing Query: {}", query);

	metrics::lock_latency measureLock("database");
	std::scoped_lock lock { databaseLock };
	measureLock.stop();

	metrics::query_latency measure(query.substr(0, 50));
retry:
	if (mysql_query(handle, query.data()) != 0) {
		g_logger().error("Query: {}", query);
		g_logger().error("Message: {}", mysql_error(handle));
		if (!isRecoverableError(mysql_errno(handle))) {
			return nullptr;
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
		goto retry;
	}

	// Retrieving results of query
	MYSQL_RES* res = mysql_store_result(handle);
	if (res != nullptr) {
		DBResult_ptr result = std::make_shared<DBResult>(res);
		if (!result->hasNext()) {
			return nullptr;
		}
		return result;
	}
	return nullptr;
}

std::string Database::escapeString(const std::string &s) const {
	std::string::size_type len = s.length();
	auto length = static_cast<uint32_t>(len);
	std::string escaped = escapeBlob(s.c_str(), length);
	if (escaped.empty()) {
		g_logger().warn("Error escaping string");
	}
	return escaped;
}

std::string Database::escapeBlob(const char* s, uint32_t length) const {
	size_t maxLength = (length * 2) + 1;

	std::string escaped;
	escaped.reserve(maxLength + 2);
	escaped.push_back('\'');

	if (length != 0) {
		std::string output(maxLength, '\0');
		size_t escapedLength = mysql_real_escape_string(handle, &output[0], s, length);
		output.resize(escapedLength);
		escaped.append(output);
	}

	escaped.push_back('\'');
	return escaped;
}

DBResult::DBResult(MYSQL_RES* res) {
	handle = res;

	int num_fields = mysql_num_fields(handle);

	const MYSQL_FIELD* fields = mysql_fetch_fields(handle);
	for (size_t i = 0; i < num_fields; i++) {
		listNames[fields[i].name] = i;
	}
	row = mysql_fetch_row(handle);
}

DBResult::~DBResult() {
	mysql_free_result(handle);
}

std::string DBResult::getString(const std::string &s) const {
	auto it = listNames.find(s);
	if (it == listNames.end()) {
		g_logger().error("Column '{}' does not exist in result set", s);
		return {};
	}
	if (row[it->second] == nullptr) {
		return {};
	}
	return std::string(row[it->second]);
}

const char* DBResult::getStream(const std::string &s, unsigned long &size) const {
	auto it = listNames.find(s);
	if (it == listNames.end()) {
		g_logger().error("Column '{}' doesn't exist in the result set", s);
		size = 0;
		return nullptr;
	}

	if (row[it->second] == nullptr) {
		size = 0;
		return nullptr;
	}

	size = mysql_fetch_lengths(handle)[it->second];
	return row[it->second];
}

uint8_t DBResult::getU8FromString(const std::string &string, const std::string &function) {
	auto result = static_cast<uint8_t>(std::atoi(string.c_str()));
	if (result > std::numeric_limits<uint8_t>::max()) {
		g_logger().error("[{}] Failed to get number value {} for tier table result, on function call: {}", __FUNCTION__, result, function);
		return 0;
	}

	return result;
}

int8_t DBResult::getInt8FromString(const std::string &string, const std::string &function) {
	auto result = static_cast<int8_t>(std::atoi(string.c_str()));
	if (result > std::numeric_limits<int8_t>::max()) {
		g_logger().error("[{}] Failed to get number value {} for tier table result, on function call: {}", __FUNCTION__, result, function);
		return 0;
	}

	return result;
}

size_t DBResult::countResults() const {
	return static_cast<size_t>(mysql_num_rows(handle));
}

bool DBResult::hasNext() const {
	return row != nullptr;
}

bool DBResult::next() {
	if (!handle) {
		g_logger().error("Database not initialized!");
		return false;
	}
	row = mysql_fetch_row(handle);
	return row != nullptr;
}

DBInsert::DBInsert(std::string insertQuery) :
	query(std::move(insertQuery)) {
	this->length = this->query.length();
}

bool DBInsert::addRow(std::string_view row) {
	const size_t rowLength = row.length();
	length += rowLength;
	auto max_packet_size = Database::getInstance().getMaxPacketSize();

	if (length > max_packet_size && !execute()) {
		return false;
	}

	if (values.empty()) {
		values.reserve(rowLength + 2);
		values.push_back('(');
		values.append(row);
		values.push_back(')');
	} else {
		values.reserve(values.length() + rowLength + 3);
		values.push_back(',');
		values.push_back('(');
		values.append(row);
		values.push_back(')');
	}
	return true;
}

bool DBInsert::addRow(std::ostringstream &row) {
	bool ret = addRow(row.str());
	row.str(std::string());
	return ret;
}

void DBInsert::upsert(const std::vector<std::string> &columns) {
	upsertColumns = columns;
}

bool DBInsert::execute() {
	if (values.empty()) {
		return true;
	}

	std::string baseQuery = this->query;
	std::string upsertQuery;

	if (!upsertColumns.empty()) {
		std::ostringstream upsertStream;
		upsertStream << " ON DUPLICATE KEY UPDATE ";
		for (size_t i = 0; i < upsertColumns.size(); ++i) {
			upsertStream << "`" << upsertColumns[i] << "` = VALUES(`" << upsertColumns[i] << "`)";
			if (i < upsertColumns.size() - 1) {
				upsertStream << ", ";
			}
		}
		upsertQuery = upsertStream.str();
	}

	std::string currentBatch = values;
	while (!currentBatch.empty()) {
		size_t cutPos = Database::MAX_QUERY_SIZE - baseQuery.size() - upsertQuery.size();
		if (cutPos < currentBatch.size()) {
			cutPos = currentBatch.rfind("),(", cutPos);
			if (cutPos == std::string::npos) {
				return false;
			}
			cutPos += 2;
		} else {
			cutPos = currentBatch.size();
		}

		std::string batchValues = currentBatch.substr(0, cutPos);
		if (batchValues.back() == ',') {
			batchValues.pop_back();
		}
		currentBatch = currentBatch.substr(cutPos);

		std::ostringstream query;
		query << baseQuery << " " << batchValues << upsertQuery;
		if (!Database::getInstance().executeQuery(query.str())) {
			return false;
		}
	}

	return true;
}
