# One-time codes (TOTP)

The box doubles as a hardware authenticator. It keeps the secrets of your
two-factor accounts, and when you pick one from its menu it puts the current
code on the Mac's clipboard, ready to paste.

It needs the Mac daemon running and connected: codes are made from the Mac's
clock, and they land on the Mac's clipboard.

## Using it

**Copy a code.** Hold the joystick for the menu, pick **OTP codes**, move to the
account and press the joystick, ✓ or ↵. The code goes to the clipboard — paste
it with ⌘V — and shows on the panel with the time it had left. ✓ **AGAIN** makes
a fresh one. When the code's window closes, the Mac clears it from the clipboard
if it is still there, so a stale code does not linger to be pasted by mistake.

**Add an account.** Menu, **Add OTP**. The panel shows a QR code, and beside it
the same Wi-Fi network spelled out — its name, its password — and an address:

1. Point your phone's camera at the QR code and tap *Join*. A laptop, or a
   phone with no camera to hand, joins by hand with the name and password shown.
   The panel says *Joined* once the device is on; most phones then open the page
   by themselves — if not, open the address shown.
2. Either paste the account's `otpauth://` link — what its setup QR code
   contains; scan it with your phone's camera or a QR app and copy the link — or
   type a name and the secret key the service shows under *"can't scan the
   code?"*.
3. **Add.** The panel confirms, and the account is in the list at once.

**Move several accounts at once.** The same **Link** field takes an
`otpauth-migration://offline?data=…` link: what the QR codes of Google
Authenticator's *Transfer accounts → Export accounts* hold, and what other
authenticator apps export to move to it. Read one of those QR codes with a QR
app that shows the text, paste the whole link, **Add**:

- every account in it is added on its own, and the page lists each with what
  became of it — added, already on the box, or why it cannot be;
- counter-based (HOTP) and MD5 accounts are skipped: the box makes time-based
  codes with SHA1, SHA256 or SHA512 only;
- the format carries no period, so every account is taken as a 30-second one,
  which is what Google Authenticator uses;
- a long list is split over several QR codes, and the page says which one this
  was — *QR code 2 of 3* — so you can add the others the same way;
- a link that is damaged or cut short adds nothing at all, rather than the half
  before the damage.

The link holds the secret of every account in it, in the clear. Once it is
added, clear it from the phone's clipboard and delete any screenshot of the
QR codes.

Press ✕ **DONE** when finished: the network disappears and the Wi-Fi goes off.
It also closes by itself after five minutes without a request.

The password is new every time **Add OTP** opens, while the network's name
stays the same. So a phone that joined once will offer the old password next
time, and fail: scanning the QR code replaces it. If you type it instead,
*forget* the ChickenMicro network on the phone first.

To finish enabling two-factor on the service, which asks for a first code: copy
one from the box as above.

**Remove an account.** On the same setup page, **Remove** beside its name.

## How it is kept safe

**What the box protects against**

- **Software on the Mac.** A secret never leaves the box: not over USB, not in a
  log, not on the setup page. And a code only exists because someone pressed a
  key on the box — nothing the Mac sends can ask for one. The daemon accepts
  nothing but 6 to 8 digits from the box, never logs a code (it is masked even
  in debug traces), and wipes it from the clipboard when it expires.
- **Anyone else on your network.** *Add OTP* does not join your Wi-Fi. It opens
  a network of its own, only while its screen is shown, with a fresh password
  that appears nowhere but on the panel — as text and as the QR code — not even
  in the log, since the Mac has Wi-Fi too. It takes one client at a time, and its forms carry a one-time token.
- **A wrong code.** At boot the firmware checks its HMAC against the RFC 6238
  test vectors, for SHA-1, SHA-256 and SHA-512. If that ever fails, codes are
  disabled, not wrong.

**What it does not protect against**

- **Someone holding the box.** Secrets are stored *unencrypted* in the ESP32's
  flash (NVS). With the board in hand and esptool, they can be read out. Treat
  the box like an unlocked phone running an authenticator app, and keep it where
  you keep your keys. ESP32 flash encryption would close this, but it is a
  one-way eFuse change that can brick a board when misconfigured, so this project
  does not turn it on.
- **Anything reading the Mac's clipboard** while a code is on it. The code is
  good for the rest of its window only.
- **The Mac's clock.** The box has no clock of its own and trusts the time the
  Mac sends. Software in control of the serial port could send a future time and
  keep the code you then make — it still needs your press, and it is the same
  limit a YubiKey's TOTP has.
- **Deleted secrets in flash.** NVS erases logically: the bytes can stay in flash
  until the page is reused. To be certain a secret is gone, erase the whole flash
  (`pio run -e <board> -t erase`), which removes every account.

## Limits

- 16 accounts; names up to 24 characters. The panel's font is ASCII, so an
  accented letter shows as `?`.
- Time-based codes (TOTP) only. Counter-based HOTP links are refused.
- SHA1, SHA256 or SHA512; 6, 7 or 8 digits; a period of 10 to 300 seconds.
- A transfer link up to 4096 characters, which is more than one of Google
  Authenticator's QR codes holds.

## When something goes wrong

| What you see | Why | What to do |
| ------------ | --- | ---------- |
| `NO CLOCK` in the list's header, no **COPY** key | the daemon has not sent the time yet | check it is running and connected; it sends the time on connect |
| the service refuses the code | the Mac's clock is off, or the code expired while typing | turn on *Set time and date automatically*; copy a fresh one |
| the phone will not join the setup Wi-Fi | a typo, or it saved the password of an earlier session — the name stays, the password changes | scan the QR code rather than typing; typing, *forget* the ChickenMicro network first, then enter the password as shown (lowercase, with its dashes) |
| *That transfer link is damaged or cut short* | part of the link was lost on the way — often its end, when it was copied | copy the whole link again; nothing from it was added |
| *QR code 1 of 3* after adding a transfer link | Google Authenticator split the list over several QR codes | read and add each of the others the same way |
| *Join failed. Try again.* on the panel | the device reached the network but left before getting an address, which is what a wrong password looks like | as above: scan the QR code |
| the network is not in the phone's list | the page closed (five minutes idle), or another device is on it — one at a time | check the panel still shows the QR code; reopen **Add OTP** |
| *Joined*, but no page opens | the phone found no internet and kept using mobile data | tap the phone's *Sign in to network* prompt, or turn mobile data off for a minute and open the address shown |
| *One-time codes are disabled on this box* | the self-test failed, or flash could not be opened | read the `[otp]` lines in the boot log |
| an account is missing after a reboot | it could not be read back from flash | the boot log names the slot; add it again |

The protocol side of all this — the `time` and `otp` frames and their rules —
is in [PROTOCOL.md §9](../protocol/PROTOCOL.md).
