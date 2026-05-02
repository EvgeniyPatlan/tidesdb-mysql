disable_query_log;
# Plugin is loaded via --plugin-load-add in suite.opt; suppress engine noise
# and switch the test DB to MySQL's utf8mb4 default collation.
ALTER DATABASE test DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci;
call mtr.add_suppression("\\[TIDESDB\\]");
call mtr.add_suppression("TIDESDB:");
enable_query_log;
