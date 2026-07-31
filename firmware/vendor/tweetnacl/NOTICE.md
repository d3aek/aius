# TweetNaCl — vendored verbatim

TweetNaCl 20140427 by Daniel J. Bernstein, Bernard van Gastel, Wesley
Janssen, Tanja Lange, Peter Schwabe and Sjaak Smetsers. **Public domain**
(the authors' stated dedication for TweetNaCl and NaCl).

- Source: <https://tweetnacl.cr.yp.to/20140427/tweetnacl.c> and
  `tweetnacl.h`, fetched 2026-07-31 over HTTPS from the primary site.
- `sha256(tweetnacl.c)` =
  `02e65bc3013ff2168983365e55906bc783c4c7e0a60d8100f17bb303a17175c4`
- `sha256(tweetnacl.h)` =
  `43f29ad721d9927b747b0100ab4160c119e7bb180c7c98a66e4bf79d31244287`

Used only for `crypto_sign_open` (Ed25519 signature verification) in the
OTA image-authentication path (`lib/aius_ota`). Nothing here runs on the
audio path, and the sign-side entry points are never called on the device.
Files are verbatim; any adaptation (placement, `randombytes` stub) lives
in the adapter, never in these files.
