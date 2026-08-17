/*
 * sms_mirror_rx.h — taking a mirrored text from COVEY and putting it in the message store.
 *
 * One entry point, deliberately, because BOTH transports end here: a CSM1 line off the radio
 * and a CSM1 line out of an HTTP response are the same record and must be folded in the same
 * way. See sms_mirror.h for why COVEY relays at all (the phone has no working TLS and cannot
 * reach the VoIP.ms API), and Storage.h's ingestMirrored() for how a duplicate is recognised.
 *
 * ⚠ THIS IS NOT A CHAT MESSAGE and must never be shown as one. On the mesh side it is
 * intercepted by prefix in meshtastic_service.cpp, in the same place and for the same reason
 * book-sync traffic is: a person did not send it to you, so it must not chime, vibrate, raise
 * an unread badge for the mesh app, or appear in the Chats list. What it does raise is the
 * SIP message state, because the text itself IS something to read.
 */
#ifndef SMS_MIRROR_RX_H
#define SMS_MIRROR_RX_H

/* Fold one CSM1 line into the SIP message store.
 *
 *   returns  1  a new text was stored
 *            0  we already had it (by VoIP.ms id, or paired with the phone's own copy)
 *           -1  the line was not a usable record, or there is no SIP account to file it under
 *
 * ⚠ NEEDS A SIP ACCOUNT, and refuses without one. A mirrored text has to be stored against a
 * correspondent URI the phone would itself produce, or replying to it would build a different
 * address and the conversation would split in two. The account supplies the host half.
 */
int smsMirrorIngestLine(const char* line);

/* True if `text` is mirror traffic — checked by prefix, before anything else, so that a
 * malformed or hostile packet is dropped rather than displayed. */
bool smsMirrorIsMirrorLine(const char* text);

#endif // SMS_MIRROR_RX_H
