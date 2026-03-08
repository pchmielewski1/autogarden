# Telegram integration plan

## Security
- Store bot name, token and chat IDs only in `include/telegram_local_config.h`.
- Keep `include/telegram_local_config.h` out of git.
- Use `include/telegram_local_config.example.h` as the tracked template.
- Never print token in Serial logs or HTTP responses.

## Implemented now
- Local compile-time Telegram config.
- Runtime activation when token and chat ID are present.
- Multi-target Telegram delivery using a comma-separated `chat_ids` list.
- Adaptive Telegram polling in low-priority NetTask.
- Telegram send with short retries.
- Chat authorization against the configured allowlist of `chat_ids`.
- Main Telegram entrypoint: `/ag`.
- Inline button menu for the main user flow.
- `/ag` opens the only supported user entry flow.
- `/water` triggers one safe watering pulse through the existing watering FSM.
- `/stop` forces all pumps OFF and aborts active watering cycles.
- Captive portal stores `bot_name`, `bot_token` and comma-separated `chat_ids`.

## Interaction types
1. Read-only status
   - `/ag` → menu główne
   - `📊 Status`
   - `📈 History`
   - `🌿 Profiles`
   - `❓ Help`
2. Safe control actions
   - `💧 Water`
   - `🛑 Stop`
   - `🪣 Refill`
   - `🏖 Vacation ON/OFF`
   - `⚙️ Mode AUTO/MANUAL`
   - `📶 WiFi`
3. Configuration changes
   - next step: dedicated menu branch
4. Outbound notifications
   - daily heartbeat
   - critical alerts
   - delivery to one, two or many configured `chat_ids`
   - future: anomaly and reservoir warnings

## Menu `/ag`
- `📊 Status`
- `📈 History`
- `🌿 Profiles`
- `❓ Help`
- `💧 Water`
- `🛑 Stop`
- `🪣 Refill`
- `🏖 Vacation ON/OFF`
- `⚙️ Mode AUTO/MANUAL`
- `📶 WiFi`

## Next phase
- Notification queue with throttling and deduplication.
- Richer `/history` based on persisted history.
- Dedicated configuration submenu.
- Replace insecure TLS with pinned CA/certificate validation.
