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
#include <cstdio>
#include <sstream>
#include <string>
#include <iostream>
#include <cstdlib>   // for std::atoi

extern "C" unsigned TB_LARGEST;

namespace search::syzygy {

bool g_use_lichess_tb = true;

std::string escape_fen(const std::string& fen) {
  std::string escaped = fen;
  size_t pos = 0;
  while ((pos = escaped.find(' ', pos)) != std::string::npos) {
    escaped.replace(pos, 1, "%20");
    pos += 3;
  }
  return escaped;
}

// ==================== CACHE (zero spam, even without half_clock guard) ====================
namespace {
  struct LichessCache {
    std::string fen;
    tb_dtz_result result = tb_dtz_result::failure();
  };
  LichessCache g_lichess_cache;
}

tb_dtz_result probe_lichess(const chess::board& bd) noexcept {
  if (!g_use_lichess_tb || bd.num_pieces() < 7 || bd.num_pieces() > 8) 
    return tb_dtz_result::failure();

  const std::string current_fen = bd.fen();
  if (current_fen == g_lichess_cache.fen) return g_lichess_cache.result;

  std::cerr << "LICHESS TB PROBE: " << current_fen << std::endl;

  std::string url = "https://tablebase.lichess.ovh/standard?fen=" + escape_fen(current_fen);
#ifdef _WIN32
  std::string cmd = "powershell -Command \"(Invoke-WebRequest -Uri '" + url + "' -UseBasicParsing).Content\"";
#else
  std::string cmd = "curl -s \"" + url + "\"";
#endif

  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) return tb_dtz_result::failure();

  std::stringstream ss;
  char buf[4096];
  while (fgets(buf, sizeof(buf), pipe)) ss << buf;
  pclose(pipe);

  std::string json = ss.str();

  // category + best move + DTZ
  std::string category;
  size_t cat_pos = json.find("\"category\":\"");
  if (cat_pos != std::string::npos) {
    cat_pos += 12;
    size_t e = json.find('"', cat_pos);
    if (e != std::string::npos) category = json.substr(cat_pos, e - cat_pos);
  }

  std::string best_uci;
  int best_dtz = 0;
  size_t moves_pos = json.find("\"moves\":");
  if (moves_pos != std::string::npos) {
    size_t uci_pos = json.find("\"uci\":\"", moves_pos);
    if (uci_pos != std::string::npos) {
      uci_pos += 7;
      size_t end = json.find('"', uci_pos);
      if (end != std::string::npos) best_uci = json.substr(uci_pos, end - uci_pos);
    }
    size_t dtz_pos = json.find("\"dtz\":", moves_pos);
    if (dtz_pos != std::string::npos) {
      dtz_pos += 6;
      best_dtz = std::atoi(json.c_str() + dtz_pos);
    }
  }

  if (best_uci.empty()) return tb_dtz_result::failure();

  const chess::move_list list = bd.generate_moves<>();
  chess::move best_move = chess::move::null();
  for (const auto& mv : list) {
    if (mv.name(bd.turn()) == best_uci) { best_move = mv; break; }
  }
  if (best_move == chess::move::null()) return tb_dtz_result::failure();

  const int wdl = (category == "win") ? TB_WIN : (category == "loss") ? TB_LOSS : TB_DRAW;

  int score = search::draw_score;
  if (wdl == TB_WIN)      score = 32000 - std::abs(best_dtz);
  else if (wdl == TB_LOSS) score = -32000 + std::abs(best_dtz);

  tb_dtz_result res{true, score, best_move};
  g_lichess_cache.fen = current_fen;
  g_lichess_cache.result = res;
  return res;
}

// ==================== ORIGINAL FUNCTIONS (unchanged) ====================
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

  return tb_wdl_result::from_value(value);
}

tb_dtz_result probe_dtz(const chess::board& bd) noexcept {
  if (bd.lat_.white.oo() || bd.lat_.white.ooo() || bd.lat_.black.oo() || bd.lat_.black.ooo()) 
    return tb_dtz_result::failure();

  // Local Fathom first
  if (bd.num_pieces() <= TB_LARGEST) {
    const unsigned int rule_50 = bd.lat_.half_clock;
    constexpr unsigned int castling_rights = 0;
    const unsigned int ep = bd.lat_.them(bd.turn()).ep_mask().any() ? bd.lat_.them(bd.turn()).ep_mask().item().index() : 0;
    const bool turn = bd.turn();

    const unsigned value = tb_probe_root(
        bd.man_.white.all().data, bd.man_.black.all().data, (bd.man_.white.king() | bd.man_.black.king()).data,
        (bd.man_.white.queen() | bd.man_.black.queen()).data, (bd.man_.white.rook() | bd.man_.black.rook()).data,
        (bd.man_.white.bishop() | bd.man_.black.bishop()).data, (bd.man_.white.knight() | bd.man_.black.knight()).data,
        (bd.man_.white.pawn() | bd.man_.black.pawn()).data, rule_50, castling_rights, ep, turn, nullptr);

    auto local = tb_dtz_result::from_value(bd, value);
    if (local.success) return local;
  }

  // Lichess TB7/TB8 on EVERY 7-8 man position (cache makes it safe)
  if (bd.num_pieces() >= 7 && bd.num_pieces() <= 8) {
    return probe_lichess(bd);
  }

  return tb_dtz_result::failure();
}

void init(const std::string& path) noexcept {
  tb_init(path.c_str());
  TB_LARGEST = 8;
  std::cerr << "=== SEER-APOLLO LICHESS TB7/TB8 ENABLED (full search probing) ===" << std::endl;
}

}  // namespace search::syzygy