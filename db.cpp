#include "ocelot.h"
#include "db.h"
#include "user.h"
#include "misc_functions.h"
#include "config.h"
#include <string>
#include <iostream>
#include <queue>
#include <unistd.h>
#include <ctime>
#include <mutex>
#include <thread>
#include <unordered_set>

#define DB_LOCK_TIMEOUT 50

mysql::mysql(config * conf) : u_active(false), t_active(false), p_active(false), s_active(false), tok_active(false),
ft_active(false), ut_active(false) {
	load_config(conf);
	if (mysql_db == "") {
		std::cout << "No database selected" << std::endl;
		return;
	}

	try {
		mysqlpp::ReconnectOption reconnect(true);
		conn.set_option(&reconnect);
		conn.connect(mysql_db.c_str(), mysql_host.c_str(), mysql_username.c_str(), mysql_password.c_str(), 0);
	} catch (const mysqlpp::Exception &er) {
		std::cout << "Failed to connect to MySQL (" << er.what() << ')' << std::endl;
		return;
	}

	if (!readonly) {
		std::cout << "Clearing xbt_files_users and resetting peer counts...";
		std::cout.flush();
		clear_peer_data();
		std::cout << "done" << std::endl;
	}
}

void mysql::load_config(config * conf) {
	mysql_db = conf->get_str("mysql_db");
	mysql_host = conf->get_str("mysql_host");
	mysql_username = conf->get_str("mysql_username");
	mysql_password = conf->get_str("mysql_password");
	readonly = conf->get_bool("readonly");
}

void mysql::reload_config(config * conf) {
	load_config(conf);
}

bool mysql::connected() {
	return conn.connected();
}

void mysql::clear_peer_data() {
	try {
		mysqlpp::Query query = conn.query("TRUNCATE xbt_files_users;");
		if (!query.exec()) {
			std::cerr << "Unable to truncate xbt_files_users!" << std::endl;
		}
		query = conn.query("UPDATE torrents SET Seeders = 0, Leechers = 0;");
		if (!query.exec()) {
			std::cerr << "Unable to reset seeder and leecher count!" << std::endl;
		}
	} catch (const mysqlpp::BadQuery &er) {
		std::cerr << "Query error in clear_peer_data: " << er.what() << std::endl;
	} catch (const mysqlpp::Exception &er) {
		std::cerr << "Query error in clear_peer_data: " << er.what() << std::endl;
	}
}

void mysql::load_torrents(torrent_list &torrents) {
	mysqlpp::Query query = conn.query("SELECT id, info_hash, free, times_completed FROM torrents ORDER BY id;");
	try {
		mysqlpp::StoreQueryResult res = query.store();
		std::unordered_set<std::string> cur_keys;
		size_t num_rows = res.num_rows();
		std::lock_guard<std::mutex> tl_lock(torrent_list_mutex);
		if (torrents.size() == 0) {
			torrents.reserve(num_rows * 1.05); // Reserve 5% extra space to prevent rehashing
		} else {
			// Create set with all currently known info hashes to remove nonexistent ones later
			cur_keys.reserve(torrents.size());
			for (auto const &it: torrents) {
				cur_keys.insert(it.first);
			}
		}
		for (size_t i = 0; i < num_rows; i++) {
			std::string info_hash;
			res[i][1].to_string(info_hash);
			if (info_hash == "") {
				continue;
			}
			mysqlpp::sql_enum free_torrent(res[i][2]);

			torrent tmp_tor;
			auto it = torrents.insert(std::pair<std::string, torrent>(info_hash, tmp_tor));
			torrent &tor = (it.first)->second;
			if (it.second) {
				tor.id = res[i][0];
				tor.balance = 0;
				tor.completed = res[i][3];
				tor.last_selected_seeder = "";
			} else {
				tor.tokened_users.clear();
				cur_keys.erase(info_hash);
			}
			if (free_torrent == "1") {
				tor.free_torrent = FREE;
			} else if (free_torrent == "2") {
				tor.free_torrent = NEUTRAL;
			} else if (free_torrent == "11") {
			    tor.free_torrent = OFF_25;
			} else if (free_torrent == "12") {
			    tor.free_torrent = OFF_50;
			} else if (free_torrent == "13") {
			    tor.free_torrent = OFF_75;
			} else {
				tor.free_torrent = NORMAL;
			}
		}
		for (auto const &info_hash: cur_keys) {
			// Remove tracked torrents that weren't found in the database
			auto it = torrents.find(info_hash);
			if (it != torrents.end()) {
				torrent &tor = it->second;
				stats.leechers -= tor.leechers.size();
				stats.seeders -= tor.seeders.size();
				for (auto &p: tor.leechers) {
					p.second.user->decr_leeching();
				}
				for (auto &p: tor.seeders) {
					p.second.user->decr_seeding();
				}
				torrents.erase(it);
			}
		}
	} catch (const mysqlpp::BadQuery &er) {
		std::cerr << "Query error in load_torrents: " << er.what() << std::endl;
		return;
	}
	std::cout << "Loaded " << torrents.size() << " torrents" << std::endl;
	load_tokens(torrents);
}

void mysql::load_users(user_list &users) {
	mysqlpp::Query query = conn.query("SELECT id, can_leech, torrent_pass, (visible='0' OR IP='127.0.0.1') AS protected FROM users WHERE enabled='yes';");
	try {
		mysqlpp::StoreQueryResult res = query.store();
		size_t num_rows = res.num_rows();
		std::unordered_set<std::string> cur_keys;
		std::lock_guard<std::mutex> ul_lock(user_list_mutex);
		if (users.size() == 0) {
			users.reserve(num_rows * 1.05); // Reserve 5% extra space to prevent rehashing
		} else {
			// Create set with all currently known user keys to remove nonexistent ones later
			cur_keys.reserve(users.size());
			for (auto const &it: users) {
				cur_keys.insert(it.first);
			}
		}
		for (size_t i = 0; i < num_rows; i++) {
			std::string passkey(res[i][2]);
			bool protect_ip = res[i][3];
			user_ptr tmp_user = std::make_shared<user>(res[i][0], res[i][1], protect_ip);
			auto it = users.insert(std::pair<std::string, user_ptr>(passkey, tmp_user));
			if (!it.second) {
				user_ptr &u = (it.first)->second;
				u->set_leechstatus(res[i][1]);
				u->set_protected(protect_ip);
				u->set_deleted(false);
				cur_keys.erase(passkey);
			}
		}
		for (auto const &passkey: cur_keys) {
			// Remove users that weren't found in the database
			auto it = users.find(passkey);
			if (it != users.end()) {
				it->second->set_deleted(true);
				users.erase(it);
			}
		}
	} catch (const mysqlpp::BadQuery &er) {
		std::cerr << "Query error in load_users: " << er.what() << std::endl;
		return;
	}
	std::cout << "Loaded " << users.size() << " users" << std::endl;
}

void mysql::load_tokens(torrent_list &torrents) {
	mysqlpp::Query query = conn.query("SELECT uf.uid, t.info_hash FROM users_freeleeches AS uf JOIN torrents AS t ON t.id = uf.tid WHERE uf.expired = '0';");
	int token_count = 0;
	try {
		mysqlpp::StoreQueryResult res = query.store();
		size_t num_rows = res.num_rows();
		std::lock_guard<std::mutex> tl_lock(torrent_list_mutex);
		for (size_t i = 0; i < num_rows; i++) {
			std::string info_hash;
			res[i][1].to_string(info_hash);
			auto it = torrents.find(info_hash);
			if (it != torrents.end()) {
				torrent &tor = it->second;
				tor.tokened_users.insert(res[i][0]);
				++token_count;
			}
		}
	} catch (const mysqlpp::BadQuery &er) {
		std::cerr << "Query error in load_tokens: " << er.what() << std::endl;
		return;
	}
	std::cout << "Loaded " << token_count << " tokens" << std::endl;
}


void mysql::load_whitelist(std::vector<std::string> &whitelist) {
	mysqlpp::Query query = conn.query("SELECT peer_id FROM xbt_client_whitelist;");
	try {
		mysqlpp::StoreQueryResult res = query.store();
		size_t num_rows = res.num_rows();
		std::lock_guard<std::mutex> wl_lock(whitelist_mutex);
		whitelist.clear();
		for (size_t i = 0; i<num_rows; i++) {
			std::string peer_id;
			res[i][0].to_string(peer_id);
			whitelist.push_back(peer_id);
		}
	} catch (const mysqlpp::BadQuery &er) {
		std::cerr << "Query error in load_whitelist: " << er.what() << std::endl;
		return;
	}
	if (whitelist.size() == 0) {
		std::cout << "Assuming no whitelist desired, disabling" << std::endl;
	} else {
		std::cout << "Loaded " << whitelist.size() << " clients into the whitelist" << std::endl;
	}
}

void mysql::record_token(const std::string &record) {
	if (update_token_buffer != "") {
		update_token_buffer += ",";
	}
	update_token_buffer += record;
}

void mysql::record_user(const std::string &record) {
	if (update_user_buffer != "") {
		update_user_buffer += ",";
	}
	update_user_buffer += record;
}

void mysql::record_torrent(const std::string &record) {
	std::lock_guard<std::mutex> tb_lock(torrent_buffer_lock);
	if (update_torrent_buffer != "") {
		update_torrent_buffer += ",";
	}
	update_torrent_buffer += record;
}

void mysql::record_free_torrent(const std::string &record) {
	std::lock_guard<std::mutex> tb_lock(free_torrent_buffer_lock);
	if (update_free_torrent_buffer != "") {
		update_free_torrent_buffer += ",";
	}
	update_free_torrent_buffer += record;
}

void mysql::record_peer(const std::string &record, const std::string &ipv4, const std::string &ipv6, int port, const std::string &peer_id, const std::string &useragent) {
	std::lock_guard<std::mutex> pb_lock(peer_queue_lock);
	if (!update_heavy_peer_buffer.empty()) {
		update_heavy_peer_buffer += ",";
	}
	// Null query for quoting
	mysqlpp::Query query(NULL);
	query << record << mysqlpp::quote << ipv4 << ',' << mysqlpp::quote << ipv6 << ',' << mysqlpp::quote << peer_id << ',' << mysqlpp::quote << useragent << "," << time(NULL) << ')';

	update_heavy_peer_buffer += query.str();
}
void mysql::record_peer(const std::string &record, const std::string &peer_id) {
	if (update_light_peer_buffer != "") {
		update_light_peer_buffer += ",";
	}
	mysqlpp::Query q = conn.query();
	q << record << mysqlpp::quote << peer_id << ',' << time(NULL) << ')';

	update_light_peer_buffer += q.str();
}

void mysql::record_snatch(const std::string &record, const std::string &ipv4, const std::string &ipv6) {
	if (update_snatch_buffer != "") {
		update_snatch_buffer += ",";
	}
	// Null query for quoting
	mysqlpp::Query query(NULL);
	query << record << ',' << mysqlpp::quote << ipv4 << ',' << mysqlpp::quote << ipv6 << ')';
	update_snatch_buffer += query.str();
}

void mysql::record_user_torrent(const std::string &record) {
    std::lock_guard<std::mutex> tb_lock(user_torrent_buffer_lock);
    if (!update_user_torrent_buffer.empty()) {
        update_user_torrent_buffer += ",";
    }
    update_user_torrent_buffer += record;
}

bool mysql::all_clear() {
	return (user_queue.empty() && torrent_queue.empty() && peer_queue.empty() && snatch_queue.empty()
	&& token_queue.empty() && free_torrent_queue.empty() && user_torrent_queue.empty());
}

void mysql::flush() {
	flush_users();
	flush_torrents();
	flush_free_torrents();
	flush_snatches();
	flush_peers();
	flush_tokens();
	flush_user_torrents();
}

void mysql::flush_users() {
	if (readonly) {
		update_user_buffer.clear();
		return;
	}
	std::string sql;
	std::lock_guard<std::mutex> uq_lock(user_queue_lock);
	size_t qsize = user_queue.size();
	if (verbose_flush || qsize > 0) {
		std::cout << "User flush queue size: " << qsize << ", next query length: " << user_queue.front().size() << std::endl;
	}
	if (update_user_buffer == "") {
		return;
	}
	sql = "INSERT INTO users_main (ID, Uploaded, Downloaded) VALUES " + update_user_buffer +
		" ON DUPLICATE KEY UPDATE Uploaded = Uploaded + VALUES(Uploaded), Downloaded = Downloaded + VALUES(Downloaded)";
	user_queue.push(sql);
	update_user_buffer.clear();
	if (u_active == false) {
		std::thread thread(&mysql::do_flush_users, this);
		thread.detach();
	}
}

void mysql::flush_torrents() {
	std::lock_guard<std::mutex> tb_lock(torrent_buffer_lock);
	if (readonly) {
		update_torrent_buffer.clear();
		return;
	}
	std::string sql;
	std::lock_guard<std::mutex> tq_lock(torrent_queue_lock);
	size_t qsize = torrent_queue.size();
	if (verbose_flush || qsize > 0) {
		std::cout << "Torrent flush queue size: " << qsize << ", next query length: " << torrent_queue.front().size() << std::endl;
	}
	if (update_torrent_buffer == "") {
		return;
	}
	sql = "INSERT INTO torrents (id,seeders,leechers,times_completed,balance) VALUES " + update_torrent_buffer +
		" ON DUPLICATE KEY UPDATE Seeders=VALUES(Seeders), Leechers=VALUES(Leechers), " +
		"times_completed=times_completed+VALUES(times_completed), balance=VALUES(balance), last_action = " +
		"IF(VALUES(seeders) > 0, NOW(), last_action)";
	torrent_queue.push(sql);
	update_torrent_buffer.clear();
	sql.clear();
	sql = "DELETE FROM torrents WHERE info_hash = ''";
	torrent_queue.push(sql);
	if (t_active == false) {
		std::thread thread(&mysql::do_flush_torrents, this);
		thread.detach();
	}
}

void mysql::flush_snatches() {
	if (readonly) {
		update_snatch_buffer.clear();
		return;
	}
	std::string sql;
	std::lock_guard<std::mutex> sq_lock(snatch_queue_lock);
	size_t qsize = snatch_queue.size();
	if (verbose_flush || qsize > 0) {
		std::cout << "Snatch flush queue size: " << qsize << ", next query length: " << snatch_queue.front().size() << std::endl;
	}
	if (update_snatch_buffer == "" ) {
		return;
	}
	sql = "INSERT INTO xbt_snatched (uid, fid, tstamp, IP, ipv6) VALUES " + update_snatch_buffer;
	snatch_queue.push(sql);
	update_snatch_buffer.clear();
	if (s_active == false) {
		std::thread thread(&mysql::do_flush_snatches, this);
		thread.detach();
	}
}

void mysql::flush_peers() {
	if (readonly) {
		update_light_peer_buffer.clear();
		update_heavy_peer_buffer.clear();
		return;
	}
	std::string sql;
	std::lock_guard<std::mutex> pq_lock(peer_queue_lock);
	size_t qsize = peer_queue.size();
	if (verbose_flush || qsize > 0) {
		std::cout << "Peer flush queue size: " << qsize << ", next query length: " << peer_queue.front().size() << std::endl;
	}

	// Nothing to do
	if (update_light_peer_buffer == "" && update_heavy_peer_buffer == "") {
		return;
	}

	if (update_heavy_peer_buffer != "") {
		// Because xfu inserts are slow and ram is not infinite we need to
		// limit this queue's size
		// xfu will be messed up if the light query inserts a new row,
		// but that's better than an oom crash
		if (qsize >= 1000) {
			peer_queue.pop();
		}
		sql = "INSERT INTO xbt_files_users (uid,fid,active,uploaded,downloaded,upspeed,downspeed,remaining,corrupt," +
			std::string("timespent,announced,ip,ipv6,peer_id,useragent,mtime) VALUES ") + update_heavy_peer_buffer +
					" ON DUPLICATE KEY UPDATE active=VALUES(active), uploaded=VALUES(uploaded), " +
					"downloaded=VALUES(downloaded), upspeed=VALUES(upspeed), " +
					"downspeed=VALUES(downspeed), remaining=VALUES(remaining), " +
					"corrupt=VALUES(corrupt), timespent=VALUES(timespent), " +
					"announced=VALUES(announced), mtime=VALUES(mtime)";
		peer_queue.push(sql);
		update_heavy_peer_buffer.clear();
		sql.clear();
	}
	if (update_light_peer_buffer != "") {
		// See comment above
		if (qsize >= 1000) {
			peer_queue.pop();
		}
		sql = "INSERT INTO xbt_files_users (uid,fid,timespent,announced,peer_id,mtime) VALUES " +
					update_light_peer_buffer +
					" ON DUPLICATE KEY UPDATE upspeed=0, downspeed=0, timespent=VALUES(timespent), " +
					"announced=VALUES(announced), mtime=VALUES(mtime)";
		peer_queue.push(sql);
		update_light_peer_buffer.clear();
		sql.clear();
	}

	if (p_active == false) {
		std::thread thread(&mysql::do_flush_peers, this);
		thread.detach();
	}
}

void mysql::flush_tokens() {
	if (readonly) {
		update_token_buffer.clear();
		return;
	}
	std::string sql;
	std::lock_guard<std::mutex> tq_lock(token_queue_lock);
	size_t qsize = token_queue.size();
	if (verbose_flush || qsize > 0) {
		std::cout << "Token flush queue size: " << qsize << ", next query length: " << token_queue.front().size() << std::endl;
	}
	if (update_token_buffer == "") {
		return;
	}
	sql = "INSERT INTO users_freeleeches (UserID, TorrentID, Downloaded) VALUES " + update_token_buffer +
		" ON DUPLICATE KEY UPDATE Downloaded = Downloaded + VALUES(Downloaded)";
	token_queue.push(sql);
	update_token_buffer.clear();
	if (tok_active == false) {
		std::thread thread(&mysql::do_flush_tokens, this);
		thread.detach();
	}
}

void mysql::flush_free_torrents() {
	std::lock_guard<std::mutex> tb_lock(free_torrent_buffer_lock);
	if (readonly) {
		update_free_torrent_buffer.clear();
		return;
	}
	std::string sql;
	std::lock_guard<std::mutex> tq_lock(free_torrent_queue_lock);
	size_t qsize = free_torrent_queue.size();
	if (verbose_flush || qsize > 0) {
		std::cout << "Free Torrent flush queue size: " << qsize << ", next query length: " << free_torrent_queue.front().size() << std::endl;
	}
	if (update_free_torrent_buffer == "") {
		return;
	}
	// TODO
	sql = "INSERT INTO users_freetorrents (UserID, TorrentID, FreeTorrent, Time, Uploaded, Downloaded) VALUES " + update_free_torrent_buffer +
		" ON DUPLICATE KEY UPDATE Uploaded = Uploaded + VALUES(Uploaded), Downloaded = Downloaded + VALUES(Downloaded), Time=VALUES(Time)";
	free_torrent_queue.push(sql);
	update_free_torrent_buffer.clear();
	if (ft_active == false) {
		std::thread thread(&mysql::do_flush_free_torrents, this);
		thread.detach();
	}
}

void mysql::flush_user_torrents() {
    std::lock_guard<std::mutex> tb_lock(user_torrent_buffer_lock);
    if (readonly) {
        update_user_torrent_buffer.clear();
        return;
    }
    std::string sql;
    std::lock_guard<std::mutex> tq_lock(user_torrent_queue_lock);
    size_t qsize = user_torrent_queue.size();
    if (verbose_flush || qsize > 0) {
        std::cout << "User torrent flush queue size: " << qsize << ", next query length: " << user_torrent_queue.front().size() << std::endl;
    }
    if (update_user_torrent_buffer.empty()) {
        return;
    }
    sql = "INSERT INTO users_torrents (uid, fid, seedtime, downloaded, uploaded, real_downloaded, real_uploaded, snatched, end_time) VALUES " + update_user_torrent_buffer +
          " ON DUPLICATE KEY UPDATE seedtime = seedtime + VALUES(seedtime), downloaded = downloaded + VALUES(downloaded), uploaded = uploaded + VALUES(uploaded), " +
          " real_downloaded = real_downloaded + VALUES(real_downloaded), real_uploaded = real_uploaded + VALUES(real_uploaded)," +
          " snatched = snatched + VALUES(snatched), end_time = now()";
    user_torrent_queue.push(sql);
    update_user_torrent_buffer.clear();
    if (!ut_active) {
        std::thread thread(&mysql::do_flush_user_torrents, this);
        thread.detach();
    }
}

void mysql::do_flush_users() {
	u_active = true;
	try {
		mysqlpp::Connection c(mysql_db.c_str(), mysql_host.c_str(), mysql_username.c_str(), mysql_password.c_str(), 0);
		while (user_queue.size() > 0) {
			try {
				std::string sql = user_queue.front();
				mysqlpp::Query query = c.query(sql);
				if (!query.exec()) {
					std::cout << "User flush failed (" << user_queue.size() << " remain)" << std::endl;
					sleep(3);
					continue;
				} else {
					std::lock_guard<std::mutex> uq_lock(user_queue_lock);
					user_queue.pop();
				}
			}
			catch (const mysqlpp::BadQuery &er) {
				std::cerr << "Query error: " << er.what() << " in flush users with a qlength: " << user_queue.front().size() << " queue size: " << user_queue.size() << std::endl;
				sleep(3);
				continue;
			} catch (const mysqlpp::Exception &er) {
				std::cerr << "Query error: " << er.what() << " in flush users with a qlength: " << user_queue.front().size() <<  " queue size: " << user_queue.size() << std::endl;
				sleep(3);
				continue;
			}
		}
	}
	catch (const mysqlpp::Exception &er) {
		std::cerr << "MySQL error in flush_users: " << er.what() << std::endl;
	}
	u_active = false;
}

void mysql::do_flush_torrents() {
	t_active = true;
	try {
		mysqlpp::Connection c(mysql_db.c_str(), mysql_host.c_str(), mysql_username.c_str(), mysql_password.c_str(), 0);
		while (torrent_queue.size() > 0) {
			try {
				std::string sql = torrent_queue.front();
				if (sql == "") {
					torrent_queue.pop();
					continue;
				}
				mysqlpp::Query query = c.query(sql);
				if (!query.exec()) {
					std::cout << "Torrent flush failed (" << torrent_queue.size() << " remain)" << std::endl;
					sleep(3);
					continue;
				} else {
					std::lock_guard<std::mutex> tq_lock(torrent_queue_lock);
					torrent_queue.pop();
				}
			}
			catch (const mysqlpp::BadQuery &er) {
				std::cerr << "Query error: " << er.what() << " in flush torrents with a qlength: " << torrent_queue.front().size() << " queue size: " << torrent_queue.size() << std::endl;
				sleep(3);
				continue;
			} catch (const mysqlpp::Exception &er) {
				std::cerr << "Query error: " << er.what() << " in flush torrents with a qlength: " << torrent_queue.front().size() << " queue size: " << torrent_queue.size() << std::endl;
				sleep(3);
				continue;
			}
		}
	}
	catch (const mysqlpp::Exception &er) {
		std::cerr << "MySQL error in flush_torrents: " << er.what() << std::endl;
	}
	t_active = false;
}

void mysql::do_flush_peers() {
	p_active = true;
	try {
		mysqlpp::Connection c(mysql_db.c_str(), mysql_host.c_str(), mysql_username.c_str(), mysql_password.c_str(), 0);
		while (peer_queue.size() > 0) {
			try {
				std::string sql = peer_queue.front();
				mysqlpp::Query query = c.query(sql);
				if (!query.exec()) {
					std::cout << "Peer flush failed (" << peer_queue.size() << " remain)" << std::endl;
					sleep(3);
					continue;
				} else {
					std::lock_guard<std::mutex> pq_lock(peer_queue_lock);
					peer_queue.pop();
				}
			}
			catch (const mysqlpp::BadQuery &er) {
				std::cerr << "Query error: " << er.what() << " in flush peers with a qlength: " << peer_queue.front().size() << " queue size: " << peer_queue.size() << std::endl;
				sleep(3);
				continue;
			} catch (const mysqlpp::Exception &er) {
				std::cerr << "Query error: " << er.what() << " in flush peers with a qlength: " << peer_queue.front().size() << " queue size: " << peer_queue.size() << std::endl;
				sleep(3);
				continue;
			}
		}
	}
	catch (const mysqlpp::Exception &er) {
		std::cerr << "MySQL error in flush_peers: " << er.what() << std::endl;
	}
	p_active = false;
}

void mysql::do_flush_snatches() {
	s_active = true;
	try {
		mysqlpp::Connection c(mysql_db.c_str(), mysql_host.c_str(), mysql_username.c_str(), mysql_password.c_str(), 0);
		while (snatch_queue.size() > 0) {
			try {
				std::string sql = snatch_queue.front();
				mysqlpp::Query query = c.query(sql);
				if (!query.exec()) {
					std::cout << "Snatch flush failed (" << snatch_queue.size() << " remain)" << std::endl;
					sleep(3);
					continue;
				} else {
					std::lock_guard<std::mutex> sq_lock(snatch_queue_lock);
					snatch_queue.pop();
				}
			}
			catch (const mysqlpp::BadQuery &er) {
				std::cerr << "Query error: " << er.what() << " in flush snatches with a qlength: " << snatch_queue.front().size() << " queue size: " << snatch_queue.size() << std::endl;
				sleep(3);
				continue;
			} catch (const mysqlpp::Exception &er) {
				std::cerr << "Query error: " << er.what() << " in flush snatches with a qlength: " << snatch_queue.front().size() << " queue size: " << snatch_queue.size() << std::endl;
				sleep(3);
				continue;
			}
		}
	}
	catch (const mysqlpp::Exception &er) {
		std::cerr << "MySQL error in flush_snatches: " << er.what() << std::endl;
	}
	s_active = false;
}

void mysql::do_flush_tokens() {
	tok_active = true;
	try {
		mysqlpp::Connection c(mysql_db.c_str(), mysql_host.c_str(), mysql_username.c_str(), mysql_password.c_str(), 0);
		while (token_queue.size() > 0) {
			try {
				std::string sql = token_queue.front();
				mysqlpp::Query query = c.query(sql);
				if (!query.exec()) {
					std::cout << "Token flush failed (" << token_queue.size() << " remain)" << std::endl;
					sleep(3);
					continue;
				} else {
					std::lock_guard<std::mutex> tq_lock(token_queue_lock);
					token_queue.pop();
				}
			}
			catch (const mysqlpp::BadQuery &er) {
				std::cerr << "Query error: " << er.what() << " in flush tokens with a qlength: " << token_queue.front().size() << " queue size: " << token_queue.size() << std::endl;
				sleep(3);
				continue;
			} catch (const mysqlpp::Exception &er) {
				std::cerr << "Query error: " << er.what() << " in flush tokens with a qlength: " << token_queue.front().size() << " queue size: " << token_queue.size() << std::endl;
				sleep(3);
				continue;
			}
		}
	}
	catch (const mysqlpp::Exception &er) {
		std::cerr << "MySQL error in flush_tokens: " << er.what() << std::endl;
	}
	tok_active = false;
}

void mysql::do_flush_free_torrents() {
	ft_active = true;
	try {
		mysqlpp::Connection c(mysql_db.c_str(), mysql_host.c_str(), mysql_username.c_str(), mysql_password.c_str(), 0);
		while (free_torrent_queue.size() > 0) {
			try {
				std::string sql = free_torrent_queue.front();
				if (sql == "") {
					std::lock_guard<std::mutex> tq_lock(free_torrent_queue_lock);
					free_torrent_queue.pop();
					continue;
				}
				mysqlpp::Query query = c.query(sql);
				if (!query.exec()) {
					std::cout << "Free Torrent flush failed (" << free_torrent_queue.size() << " remain)" << std::endl;
					sleep(3);
					continue;
				} else {
					std::lock_guard<std::mutex> tq_lock(free_torrent_queue_lock);
					free_torrent_queue.pop();
				}
			}
			catch (const mysqlpp::BadQuery &er) {
				std::cerr << "Query error: " << er.what() << " in flush free torrents with a qlength: " << free_torrent_queue.front().size() << " queue size: " << free_torrent_queue.size() << std::endl;
				sleep(3);
				continue;
			} catch (const mysqlpp::Exception &er) {
				std::cerr << "Query error: " << er.what() << " in flush free torrents with a qlength: " << free_torrent_queue.front().size() << " queue size: " << free_torrent_queue.size() << std::endl;
				sleep(3);
				continue;
			}
		}
	}
	catch (const mysqlpp::Exception &er) {
		std::cerr << "MySQL error in flush_free_torrents: " << er.what() << std::endl;
	}
	ft_active = false;
}

void mysql::do_flush_user_torrents() {
    ut_active = true;
    try {
        mysqlpp::Connection c(mysql_db.c_str(), mysql_host.c_str(), mysql_username.c_str(), mysql_password.c_str(), 0);
        while (!user_torrent_queue.empty()) {
            try {
                std::string sql = user_torrent_queue.front();
                if (sql.empty()) {
                    std::lock_guard<std::mutex> tq_lock(user_torrent_queue_lock);
                    user_torrent_queue.pop();
                    continue;
                }
                mysqlpp::Query query = c.query(sql);
                if (!query.exec()) {
                    std::cout << "User torrent flush failed (" << user_torrent_queue.size() << " remain)" << std::endl;
                    sleep(3);
                    continue;
                } else {
                    std::lock_guard<std::mutex> tq_lock(user_torrent_queue_lock);
                    user_torrent_queue.pop();
                }
            }
            catch (const mysqlpp::BadQuery &er) {
                std::cerr << "Query error: " << er.what() << " in flush user torrents with a qlength: " << user_torrent_queue.front().size() << " queue size: " << user_torrent_queue.size() << std::endl;
                sleep(3);
                continue;
            } catch (const mysqlpp::Exception &er) {
                std::cerr << "Query error: " << er.what() << " in flush user torrents with a qlength: " << user_torrent_queue.front().size() << " queue size: " << user_torrent_queue.size() << std::endl;
                sleep(3);
                continue;
            }
        }
    }
    catch (const mysqlpp::Exception &er) {
        std::cerr << "MySQL error in flush_user_torrents: " << er.what() << std::endl;
    }
    ut_active = false;
}
