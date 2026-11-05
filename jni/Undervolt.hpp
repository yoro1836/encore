/*
 * Copyright (C) 2024-2026 Rem01Gaming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

/**
 * Exynos undervolting through the stock kernel's ACPM voltage margin
 * interface (/sys/kernel/percent_margin/<domain>_margin_percent).
 *
 * Boot loop guard: a pending marker is written (and synced) before new
 * margins are applied and removed once the device stayed up for a while.
 * If the daemon starts and finds the marker, the previous boot crashed
 * with unconfirmed margins, so they are reset to stock.
 */
namespace Undervolt {

/**
 * @brief Whether the kernel exposes the voltage margin interface.
 */
[[nodiscard]] bool is_supported();

/**
 * @brief Run once at daemon start: handle a crashed previous boot and
 *        apply the configured margins.
 */
void init();

/**
 * @brief Re-apply margins if the undervolt config changed.
 */
void on_config_changed();

} // namespace Undervolt
