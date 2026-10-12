/*
  Seer is a UCI chess engine by Connor McMonigle
  Copyright (C) 2021-2023  Connor McMonigle

  Seer is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Seer is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <search/syzygy.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace search::syzygy {

// Online 7-8-man tablebase (Lichess API) for the root position only: asked once per move before the search, with a
// time limit, off unless UCI OnlineTB is set. Windows: WinHTTP; elsewhere: curl.
namespace {

std::string url_escape_fen(const std::string& fen) {
  std::string out{};
  for (const char c : fen) {
    if (c == ' ') {
      out += "%20";
    } else {
      out += c;
    }
  }
  return out;
}

#ifdef _WIN32
bool http_get(const std::string& path, const int& timeout_ms, std::string& body) noexcept {
  static std::mutex session_mutex{};
  static HINTERNET session = nullptr;
  {
    std::lock_guard<std::mutex> lock(session_mutex);
    if (!session) { session = WinHttpOpen(L"Seer-Apollo", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0); }
    if (!session) { return false; }
  }
  HINTERNET connect = WinHttpConnect(session, L"tablebase.lichess.ovh", INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!connect) { return false; }
  const std::wstring wide_path(path.begin(), path.end());
  HINTERNET request =
      WinHttpOpenRequest(connect, L"GET", wide_path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  bool ok = false;
  if (request) {
    WinHttpSetTimeouts(request, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
      DWORD status = 0;
      DWORD status_size = sizeof(status);
      WinHttpQueryHeaders(
          request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
      if (status == 200) {
        ok = true;
        for (;;) {
          DWORD available = 0;
          if (!WinHttpQueryDataAvailable(request, &available)) {
            ok = false;
            break;
          }
          if (available == 0) { break; }
          std::string chunk(available, '\0');
          DWORD read = 0;
          if (!WinHttpReadData(request, chunk.data(), available, &read)) {
            ok = false;
            break;
          }
          body.append(chunk.data(), read);
          if (body.size() > (1u << 20)) {
            ok = false;
            break;
          }
        }
      }
    }
    WinHttpCloseHandle(request);
  }
  WinHttpCloseHandle(connect);
  return ok;
}
#else
bool http_get(const std::string& path, const int& timeout_ms, std::string& body) noexcept {
  const std::string cmd = "curl -s -f --max-time " + std::to_string(std::max(1, timeout_ms) / 1000.0) + " \"https://tablebase.lichess.ovh" + path + "\"";
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) { return false; }
  char buf[8192];
  std::size_t n = 0;
  while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) { body.append(buf, n); }
  return pclose(pipe) == 0 && !body.empty();
}
#endif

// value of "key" in the given part of the JSON: a string ("...") or a number / null
std::string json_value(const std::string& json, const std::string& key, const std::size_t& from, const std::size_t& to) {
  const std::size_t k = json.find("\"" + key + "\":", from);
  if (k == std::string::npos || k >= to) { return {}; }
  std::size_t v = k + key.size() + 3;
  while (v < json.size() && json[v] == ' ') { ++v; }
  if (v < json.size() && json[v] == '"') {
    const std::size_t e = json.find('"', v + 1);
    return e == std::string::npos ? std::string{} : json.substr(v + 1, e - v - 1);
  }
  std::size_t e = v;
  while (e < json.size() && json[e] != ',' && json[e] != '}' && json[e] != ']') { ++e; }
  return json.substr(v, e - v);
}

online_result parse_online(const std::string& json, const chess::board& bd) noexcept {
  const std::size_t moves_pos = json.find("\"moves\":");
  if (moves_pos == std::string::npos) { return {}; }

  online_result result{};
  result.category = json_value(json, "category", 0, moves_pos);
  const std::string dtz = json_value(json, "dtz", 0, moves_pos);
  result.dtz = (dtz.empty() || dtz == "null") ? 0 : std::atoi(dtz.c_str());

  // Only categories that are exact: "maybe-*" and "unknown" are left to the search.
  static const char* exact[] = {"win", "syzygy-win", "cursed-win", "draw", "blessed-loss", "syzygy-loss", "loss"};
  if (std::none_of(std::begin(exact), std::end(exact), [&](const char* c) { return result.category == c; })) { return {}; }

  const std::string best_uci = json_value(json, "uci", moves_pos, json.size());  // moves are sorted best first
  for (const auto& mv : bd.generate_moves<>()) {
    if (mv.name(bd.turn()) == best_uci) {
      result.move = mv;
      result.success = true;
      return result;
    }
  }
  return {};
}

struct online_cache {
  std::mutex mutex{};
  std::unordered_map<std::string, online_result> entries{};
};

online_cache& cache() {
  static online_cache instance{};
  return instance;
}

}  // namespace

online_result probe_online_root(const chess::board& bd, const int& timeout_ms) noexcept {
  const std::size_t men = bd.num_pieces();
  if (men < 7 || men > 8 || timeout_ms <= 0) { return {}; }
  if (bd.lat_.white.oo() || bd.lat_.white.ooo() || bd.lat_.black.oo() || bd.lat_.black.ooo()) { return {}; }

  const std::string fen = bd.fen();
  {
    std::lock_guard<std::mutex> lock(cache().mutex);
    if (const auto it = cache().entries.find(fen); it != cache().entries.end()) { return it->second; }
  }

  // The request runs on its own thread so this call never waits longer than timeout_ms; a late answer is dropped.
  struct shared_state {
    std::mutex mutex{};
    std::condition_variable done_cv{};
    bool done{false};
    bool ok{false};
    std::string body{};
  };
  auto state = std::make_shared<shared_state>();
  const std::string path = "/standard?fen=" + url_escape_fen(fen);
  const auto start = std::chrono::steady_clock::now();
  std::thread([state, path, timeout_ms] {
    std::string body{};
    const bool ok = http_get(path, timeout_ms, body);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->ok = ok;
    state->body = std::move(body);
    state->done = true;
    state->done_cv.notify_all();
  }).detach();

  std::unique_lock<std::mutex> lock(state->mutex);
  if (!state->done_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return state->done; }) || !state->ok) { return {}; }

  online_result result = parse_online(state->body, bd);
  result.elapsed_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
  if (result.success) {
    std::lock_guard<std::mutex> cache_lock(cache().mutex);
    if (cache().entries.size() > 4096) { cache().entries.clear(); }
    cache().entries[fen] = result;
  }
  return result;
}

tb_dtz_result tb_dtz_result::from_value(const chess::board& bd, const unsigned int& value) noexcept {
  auto is_same_promo = [](const chess::move& mv, const int& promo) {
    constexpr int num_pieces = 6;
    return ((!mv.is_promotion() && promo == 0) || (mv.is_promotion() && (num_pieces - promo - 1) == static_cast<int>(mv.promotion())));
  };

  if (value == TB_RESULT_FAILED || value == TB_RESULT_CHECKMATE || value == TB_RESULT_STALEMATE) { return failure(); }
  const int wdl = TB_GET_WDL(value);

  const chess::move dtz_move = [&] {
    const chess::move_list list = bd.generate_moves<>();
    const int promo = TB_GET_PROMOTES(value);
    const int from = TB_GET_FROM(value);
    const int to = TB_GET_TO(value);
    const auto it = std::find_if(list.begin(), list.end(), [&](const chess::move& mv) {
      return mv.from().index() == from && mv.to().index() == to && is_same_promo(mv, promo);
    });
    if (it != list.end()) { return *it; }
    return chess::move::null();
  }();

  if (dtz_move == chess::move::null()) { return failure(); }

  if (wdl == TB_WIN) { return tb_dtz_result{true, search::tb_win_score, dtz_move}; }
  if (wdl == TB_LOSS) { return tb_dtz_result{true, search::tb_loss_score, dtz_move}; }
  return tb_dtz_result{true, search::draw_score, dtz_move};
}

tb_wdl_result probe_wdl(const chess::board& bd) noexcept {
  if (bd.num_pieces() > TB_LARGEST || bd.lat_.half_clock != 0) { return tb_wdl_result::failure(); }
  if (bd.lat_.white.oo() || bd.lat_.white.ooo() || bd.lat_.black.oo() || bd.lat_.black.ooo()) { return tb_wdl_result::failure(); }

  constexpr unsigned int rule_50 = 0;
  constexpr unsigned int castling_rights = 0;
  const unsigned int ep = bd.lat_.them(bd.turn()).ep_mask().any() ? bd.lat_.them(bd.turn()).ep_mask().item().index() : 0;
  const bool turn = bd.turn();

  const unsigned value = tb_probe_wdl(
      bd.man_.white.all().data, bd.man_.black.all().data, (bd.man_.white.king() | bd.man_.black.king()).data,
      (bd.man_.white.queen() | bd.man_.black.queen()).data, (bd.man_.white.rook() | bd.man_.black.rook()).data,
      (bd.man_.white.bishop() | bd.man_.black.bishop()).data, (bd.man_.white.knight() | bd.man_.black.knight()).data,
      (bd.man_.white.pawn() | bd.man_.black.pawn()).data, rule_50, castling_rights, ep, turn);

  // A cursed win / blessed loss is a draw under the 50-move rule, not a failed probe.
  if (value == TB_CURSED_WIN || value == TB_BLESSED_LOSS) { return tb_wdl_result{true, wdl_type::draw}; }
  return tb_wdl_result::from_value(value);
}

tb_dtz_result probe_dtz(const chess::board& bd) noexcept {
  if (bd.num_pieces() > TB_LARGEST) { return tb_dtz_result::failure(); }
  if (bd.lat_.white.oo() || bd.lat_.white.ooo() || bd.lat_.black.oo() || bd.lat_.black.ooo()) { return tb_dtz_result::failure(); }

  const unsigned int rule_50 = bd.lat_.half_clock;
  constexpr unsigned int castling_rights = 0;
  const unsigned int ep = bd.lat_.them(bd.turn()).ep_mask().any() ? bd.lat_.them(bd.turn()).ep_mask().item().index() : 0;
  const bool turn = bd.turn();

  const unsigned value = tb_probe_root(
      bd.man_.white.all().data, bd.man_.black.all().data, (bd.man_.white.king() | bd.man_.black.king()).data,
      (bd.man_.white.queen() | bd.man_.black.queen()).data, (bd.man_.white.rook() | bd.man_.black.rook()).data,
      (bd.man_.white.bishop() | bd.man_.black.bishop()).data, (bd.man_.white.knight() | bd.man_.black.knight()).data,
      (bd.man_.white.pawn() | bd.man_.black.pawn()).data, rule_50, castling_rights, ep, turn, nullptr);

  return tb_dtz_result::from_value(bd, value);
}

void init(const std::string& path) noexcept { tb_init(path.c_str()); }

}  // namespace search::syzygy
