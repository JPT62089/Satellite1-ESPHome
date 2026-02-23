#pragma once

#include "esphome/core/automation.h"
#include "sendspin_client.h"

namespace esphome {
namespace sendspin {

template<typename... Ts> class EnableAction : public Action<Ts...>, public Parented<SendspinClient> {
 public:
  void play(const Ts &...x) override { this->parent_->enable(); }
};

template<typename... Ts> class DisableAction : public Action<Ts...>, public Parented<SendspinClient> {
 public:
  void play(const Ts &...x) override { this->parent_->disable(); }
};

template<typename... Ts> class PlayPauseAction : public Action<Ts...>, public Parented<SendspinClient> {
 public:
  void play(const Ts &...x) override { this->parent_->action_play_pause(); }
};

template<typename... Ts> class NextAction : public Action<Ts...>, public Parented<SendspinClient> {
 public:
  void play(const Ts &...x) override { this->parent_->action_next(); }
};

template<typename... Ts> class PreviousAction : public Action<Ts...>, public Parented<SendspinClient> {
 public:
  void play(const Ts &...x) override { this->parent_->action_previous(); }
};

template<typename... Ts> class SetVolumeAction : public Action<Ts...>, public Parented<SendspinClient> {
 public:
  TEMPLATABLE_VALUE(float, volume)
  void play(const Ts &...x) override { this->parent_->action_set_volume(this->volume_.value(x...)); }
};

}  // namespace sendspin
}  // namespace esphome
