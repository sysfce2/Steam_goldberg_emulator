**This is currently available for Windows only**  

Place your overlay audio files here.  
Specific-type files take priority over the generic fallback (`overlay_friend_notification.wav`).

**Generic fallbacks (used when the specific file is absent):**
* `overlay_friend_notification.wav`: fallback for all friend/lobby notification types
* `overlay_achievement_notification.wav`: fallback for `overlay_achievement_progress.wav`
* `overlay_lobby_join_response.wav`: fallback for `overlay_lobby_join_accepted.wav` and `overlay_lobby_join_denied.wav`

**Per-type files (override the generic fallback individually):**
* `overlay_invite_notification.wav`: played when a friend sends a game invite
* `overlay_chat_notification.wav`: played when a friend sends a chat message
* `overlay_auto_accept_notification.wav`: played when an invite is automatically accepted
* `overlay_lobby_join_request.wav`: played when someone requests to join your lobby
* `overlay_lobby_join_accepted.wav`: played when your lobby join request is accepted
* `overlay_lobby_join_denied.wav`: played when your lobby join request is denied
* `overlay_lobby_kicked.wav`: played when you are kicked from a lobby
* `overlay_friend_lobby.wav`: played when a friend enters a lobby
* `overlay_lobby_status.wav`: played on lobby/server status changes (created, closed, server started/stopped)
* `overlay_achievement_progress.wav`: played when achievement progress is updated (not yet unlocked)
* `overlay_achievement_notification.wav`: played when an achievement is unlocked

