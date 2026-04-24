// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
#include <chesserazade/game.hpp>

#include <chesserazade/fen.hpp>

#include <cassert>
#include <utility>

namespace chesserazade {

namespace {

/// Build a starting position from the standard FEN. Used by the
/// default constructor. The FEN is known-valid, so we assert rather
/// than propagate an error — a parsing failure here would be an
/// internal bug, not a user-facing condition.
Board8x8Mailbox default_starting_position() {
    auto r = Board8x8Mailbox::from_fen(STARTING_POSITION_FEN);
    assert(r.has_value() && "STARTING_POSITION_FEN must parse");
    return std::move(*r);
}

} // namespace

Game::Game()
    : starting_(default_starting_position()),
      current_(starting_) {}

Game::Game(Board8x8Mailbox start)
    : starting_(std::move(start)),
      current_(starting_) {}

void Game::play_move(const Move& m) {
    current_.make_move(m);
    moves_.push_back(m);
}

bool Game::undo_move() noexcept {
    if (moves_.empty()) {
        return false;
    }
    const Move m = moves_.back();
    moves_.pop_back();
    current_.unmake_move(m);
    return true;
}

void Game::reset_to_start() {
    current_ = starting_;
    moves_.clear();
}

} // namespace chesserazade
