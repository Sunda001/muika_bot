// SPDX-License-Identifier: GPL-2.0-only

#include "muika/modules/jqftu/Session.hpp"
#include "muika/modules/jqftu/internal.hpp"

#include <unordered_map>
#include <stdexcept>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <dirent.h>

using json = nlohmann::json;

namespace muika {
namespace modules {
namespace jqftu {

static std::unordered_map<int64_t, Session *> g_sessions;
static std::mutex g_sessions_mutex;

struct curl_resp {
	char	*data;
	size_t	size;
};

static size_t curl_cb(void *data, size_t size, size_t nmemb, void *userp)
{
	struct curl_resp *cr = (struct curl_resp *)userp;
	size_t real_size = size * nmemb;
	char *tmp;
	
	tmp = (char *)realloc(cr->data, cr->size + real_size + 1);
	if (!tmp)
		return 0;

	cr->data = tmp;
	memcpy(cr->data + cr->size, data, real_size);
	cr->size += real_size;
	cr->data[cr->size] = '\0';
	return real_size;
}

static char *generate_latex_text(const char *kanji)
{
	static const char data_template[] = "{\"content\":\"\\\\documentclass[32pt]{article}\\n\\\\usepackage{CJKutf8}\\n\\\\thispagestyle{empty}\\n\\\\begin{document}\\n\\\\begin{CJK}{UTF8}{min}\\n%s\\n\\\\end{CJK}\\n\\\\end{document}\\n\",\"d\":800,\"border\":\"100x80\",\"bcolor\":\"white\"}";
	struct curl_slist *headers = NULL;
	struct curl_resp cr;
	size_t body_len;
	char *req_body;
	CURLcode res;
	CURL *curl;

	body_len = strlen(data_template) + strlen(kanji);
	req_body = (char *)malloc(body_len + 1);
	if (!req_body)
		return NULL;

	curl = curl_easy_init();
	if (!curl) {
		free(req_body);
		return NULL;
	}

	cr.data = NULL;
	cr.size = 0;
	snprintf(req_body, body_len + 1, data_template, kanji);

	headers = curl_slist_append(headers, "Content-Type: text/plain;charset=UTF-8");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_URL, "https://latex.teainside.org/api.php?action=tex2png_no_op");
	curl_easy_setopt(curl, CURLOPT_POST, 1);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(req_body));
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &cr);
	res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	free(req_body);

	if (res != CURLE_OK) {
		free(cr.data);
		return NULL;
	}

	return cr.data;
}

static char *generate_text_image(const char *kanji)
{
	static const char url_template[] = "https://latex.teainside.org/api.php?action=file&type=png&hash=";
	char *json_str = generate_latex_text(kanji);
	size_t len;
	char *ret;
	json j;

	if (!json_str)
		return NULL;

	try {
		j = json::parse(json_str);
	} catch (json::parse_error &e) {
		goto out_err;
	}

	if (!j.is_object())
		goto out_err;

	if (j.find("res") == j.end())
		goto out_err;

	if (!j["res"].is_string())
		goto out_err;

	len = strlen(url_template) + j["res"].get<std::string>().length();
	ret = (char *)malloc(len + 1);
	if (!ret)
		goto out_err;

	snprintf(ret, len + 1, "%s%s", url_template, j["res"].get<std::string>().c_str());
	free(json_str);
	return ret;

out_err:
	free(json_str);
	return NULL;
}

Session::Session(Muika &m, int64_t chat_id, const std::string &deck_name):
	m_(m),
	chat_id_(chat_id)
{
	ref_count_ = 0;
	deck_ = Deck::createDeck(deck_name);
}

Session::~Session(void)
{
}

inline bool Session::sendCard(void)
{
	std::string q, i;
	char *url;

	q = current_card_->getQuestion();
	url = generate_text_image(q.c_str());
	if (!url) {
		m_.getBot().getApi().sendMessage(chat_id_, "Failed to generate image, stopping session...");
		return false;
	}

	i = current_card_->getQuestionInfo() + "\n\nTimeout: " + std::to_string(timeout_) + " seconds";
	auto ret = m_.getBot().getApi().sendPhoto(chat_id_, url, i, last_msg_id_);
	free(url);
	if (!ret) {
		m_.getBot().getApi().sendMessage(chat_id_, "Failed to send image, stopping session...");
		return false;
	}

	last_msg_id_ = ret->messageId;
	return true;
}

inline void Session::sendFailMessage(const std::string &msg)
{
	m_.getBot().getApi().sendMessage(chat_id_,
		msg + "\n\n" + current_card_->getAnswerInfo(),
		false, last_msg_id_);
}

inline std::string htmlspecialchars(const std::string &str)
{
	std::string ret = "";

	for (auto &c: str) {
		switch (c) {
		case '&':
			ret += "&amp;";
			break;
		case '<':
			ret += "&lt;";
			break;
		case '>':
			ret += "&gt;";
			break;
		case '"':
			ret += "&quot;";
			break;
		case '\'':
			ret += "&apos;";
			break;
		default:
			ret += c;
			break;
		}
	}
	return ret;
}

inline void Session::sendFinishMessage(void)
{
	std::string score;
	uint32_t i = 1;

	/*
	 * Sort scores by point
	 */
	std::vector<std::pair<int64_t, Score>> scores_vec(scores_.begin(), scores_.end());
	std::sort(scores_vec.begin(), scores_vec.end(), [](const auto &a, const auto &b) {
		return a.second.point_ > b.second.point_;
	});

	score = "Session finished!\n\n";
	for (auto &s: scores_vec) {
		std::string name_link;
		std::string line;

		name_link = "<a href=\"tg://user?id=" + std::to_string(s.first) + "\">" +
			htmlspecialchars(s.second.full_name_) + "</a>";

		score += std::to_string(i++) + ". " + name_link + ": " +
			 std::to_string(s.second.point_) + " point";

		if (s.second.point_ > 1)
			score += "s";

		score += "\n";
	}

	if (current_card_)
		sendFailMessage("Game is stopped!");

	m_.getBot().getApi().sendMessage(chat_id_, score, true, 0, nullptr, "HTML");
}

inline void Session::worker(void)
{
	ref_count_++;
	std::unique_lock<std::mutex> lock(mutex_);

	pr_debug("Shuffling deck...\n");
	deck_->shuffle();

	while (1) {
		if (should_stop_)
			break;

		if (deck_->isFinished())
			break;

		current_card_ = deck_->draw();
		if (!current_card_)
			break;

		if (!sendCard())
			break;

		if (cond_.wait_for(lock, std::chrono::seconds(timeout_)) == std::cv_status::timeout) {
			sendFailMessage("Time's up!");
			current_card_ = nullptr;
		}

		if (!deck_->isFinished() && !should_stop_)
			cond_.wait_for(lock, std::chrono::seconds(next_delay_));

		serializeSessions(chat_id_);
	}

	sendFinishMessage();
	lock.unlock();
	deleteSession(chat_id_);
	if (ref_count_-- == 1)
		delete this;
}

void Session::_start(void)
{
	std::unique_lock<std::mutex> lock(mutex_);

	try {
		worker_ = std::thread(&Session::worker, this);
	} catch (const std::exception &e) {
		m_.getBot().getApi().sendMessage(
			chat_id_,
			"Failed to start session: " + std::string(e.what())
		);
		return;
	}

	worker_.detach();
}

void Session::start(void)
{
	_start();
	m_.getBot().getApi().sendMessage(chat_id_, "Session started!");
}

void Session::stop(void)
{
	std::unique_lock<std::mutex> lock(mutex_);
	should_stop_ = true;
	cond_.notify_one();
}

void Session::answer(const TgBot::Message::Ptr &msg)
{
	std::unique_lock<std::mutex> lock(mutex_);
	std::string reply, answer = msg->text;
	Score *s = nullptr;

	if (!current_card_ || !current_card_->answer(answer))
		return;

	auto it = scores_.find(msg->from->id);
	if (it == scores_.end()) {
		std::string full_name = msg->from->firstName;
		if (msg->from->lastName.length())
			full_name += " " + msg->from->lastName;
		scores_.emplace(msg->from->id, Score{1, full_name, msg->from->username});
		s = &scores_.at(msg->from->id);
	} else {
		s = &it->second;
		s->point_++;
	}

	reply =	"Correct!\n"
		"Your point is: " + std::to_string(s->point_) + "\n\n" +
		current_card_->getAnswerInfo();

	m_.getBot().getApi().sendMessage(chat_id_, reply, false, msg->messageId);
	current_card_ = nullptr;
	cond_.notify_one();
}

void Session::setTimeout(uint32_t timeout, bool skip)
{
	std::unique_lock<std::mutex> lock(mutex_);

	timeout_ = timeout;
	if (skip)
		cond_.notify_one();
}

void Session::setNextDelay(uint32_t next_delay, bool skip)
{
	std::unique_lock<std::mutex> lock(mutex_);

	next_delay_ = next_delay;
	if (skip)
		cond_.notify_one();
}

// static
Session *Session::getSession(int64_t chat_id)
{
	std::unique_lock<std::mutex> lock(g_sessions_mutex);
	Session *sess;

	auto it = g_sessions.find(chat_id);
	if (it == g_sessions.end())
		return nullptr;

	sess = it->second;
	sess->ref_count_++;
	return sess;
}

// static
void Session::putSession(Session *sess)
{
	if (sess->ref_count_-- == 1)
		delete sess;
}

// static
Session *Session::createSession(Muika &m, int64_t chat_id, const std::string &deck_name)
{
	std::unique_lock<std::mutex> lock(g_sessions_mutex);
	Session *sess = nullptr;

	auto it = g_sessions.find(chat_id);
	if (it != g_sessions.end())
		return nullptr;

	try {
		sess = new Session(m, chat_id, deck_name);
		g_sessions.emplace(chat_id, sess);
		sess->ref_count_++;
	} catch (const std::exception &e) {
		delete sess;
		sess = nullptr;
	}
	return sess;
}

// static
void Session::deleteSession(int64_t chat_id)
{
	std::unique_lock<std::mutex> lock(g_sessions_mutex);

	auto it = g_sessions.find(chat_id);
	if (it == g_sessions.end())
		return;

	g_sessions.erase(it);
}

static char *load_str_from_file(const char *file_handle)
{
	size_t size, read_size;
	char *ret, path[4096];
	FILE *fp;

	snprintf(path, sizeof(path), "./storage/jqftu/sessions/%s", file_handle);
	fp = fopen(path, "rb");
	if (!fp)
		return NULL;

	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	ret = (char *)malloc(size + 1);
	if (!ret) {
		fclose(fp);
		return NULL;
	}

	read_size = fread(ret, 1, size, fp);
	fclose(fp);

	if (read_size != size) {
		free(ret);
		return NULL;
	}

	ret[size] = '\0';
	return ret;
}

// static 
void Session::initSessionFromJson(Muika &m, void *json_ptr)
{
	json &j = *(json *)json_ptr;
	std::string deck_name;
	int64_t chat_id;
	Session *s;

	if (!j.is_object())
		return;

	if (j.find("chat_id") == j.end() || !j["chat_id"].is_number_integer())
		return;

	if (j.find("deck_name") == j.end() || !j["deck_name"].is_string())
		return;

	if (j.find("scores") == j.end() || !j["scores"].is_array())
		return;

	deck_name = j["deck_name"].get<std::string>();
	chat_id = j["chat_id"].get<int64_t>();
	s = Session::createSession(m, chat_id, deck_name);
	if (!s)
		return;

	for (auto &score: j["scores"].items()) {
		auto &so = score.value();

		if (!so.is_object())
			continue;

		if (so.find("user_id") == so.end() || !so["user_id"].is_number_integer())
			continue;

		if (so.find("full_name") == so.end() || !so["full_name"].is_string())
			continue;

		if (so.find("username") == so.end() || !so["username"].is_string())
			continue;

		if (so.find("point") == so.end() || !so["point"].is_number_integer())
			continue;

		s->scores_.emplace(so["user_id"].get<int64_t>(),
				   Score{so["point"].get<uint32_t>(),
					 so["full_name"].get<std::string>(),
					 so["username"].get<std::string>()});
	}

	s->_start();
}

// static
void Session::init(Muika &m)
{
	DIR *dir;

	curl_global_init(CURL_GLOBAL_ALL);

	dir = opendir("./storage/jqftu/sessions");
	if (!dir)
		return;

	while (1) {
		struct dirent *p;
		char *json_str;
		const char *n;
		json j;

		p = readdir(dir);
		if (!p)
			break;

		n = p->d_name;
		if (n[0] == 's' && n[1] == '_') {
			json_str = load_str_from_file(p->d_name);
			if (!json_str)
				continue;

			try {
				j = json::parse(json_str);
			} catch (json::parse_error &e) {
				free(json_str);
				continue;
			}

			initSessionFromJson(m, &j);
			free(json_str);
		}
	}

	closedir(dir);
}

// static 
void Session::serializeSessions(int64_t key)
{
	std::unique_lock<std::mutex> lock(g_sessions_mutex);
	auto it = g_sessions.find(key);
	std::string json_str;
	char path[4096];
	size_t len;
	FILE *fp;
	json j;

	if (it == g_sessions.end())
		return;

	j["chat_id"] = it->second->chat_id_;
	j["deck_name"] = it->second->deck_->getName();
	j["scores"] = json::array();
	for (auto &s: it->second->scores_) {
		json so;

		so["user_id"] = s.first;
		so["full_name"] = s.second.full_name_;
		so["username"] = s.second.username_;
		so["point"] = s.second.point_;
		j["scores"].push_back(so);
	}

	json_str = j.dump();
	snprintf(path, sizeof(path), "./storage/jqftu/sessions/s_%lld.json", (long long)key);
	fp = fopen(path, "wb");
	if (!fp)
		return;

	len = fwrite(json_str.c_str(), 1, json_str.length(), fp);
	fclose(fp);
	if (len != json_str.length())
		remove(path);
}

} /* namespace muika::modules::jqftu */
} /* namespace muika::modules */
} /* namespace muika */
