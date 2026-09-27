# The OTA manifest

A single static JSON file served over HTTPS. The device does all the matching,
so hosting it needs no server-side logic — any static file server will do.

`docs/ota_manifest.example.json` is a working example. Point the device's
**OTA manifest URL** at your copy (the `ota_url` config key: the Home Assistant
text entity, the bulk config document, or `NVS_DEFAULT_OTA_URL` in the
gitignored `include/credentials.local.h`). An empty URL disables update checks
entirely.

> **State:** the decision layer (`main/ota_policy.c`) is implemented and
> host-tested, and everything below describes what it actually does. The
> fetch/apply layers (`ota_flow.c`, `ota.c` — plan tasks 10 and 11) are not
> written yet, so nothing retrieves this file on-device today.

---

## The top level is an array of schema blocks

```json
[ { "schema": 1, ... } ]
```

Not an object. A top level that is not an array is `bad_manifest`.

Each block carries a distinct integer `schema`. The device selects the
**highest** block it understands — `OTA_SCHEMA_MAX`, currently **1** — and
ignores everything else. That is what makes a rolling upgrade work from one
file: when the format changes, a `schema: 2` block is added *alongside* the
schema-1 block rather than replacing it, so firmware that predates the change
keeps updating instead of being stranded. The schema-1 block is deleted only
once Home Assistant shows the whole fleet has caught up.

Selection is strict, and the strictness is deliberate:

| In the file | What happens |
| --- | --- |
| `"schema": 1` | selected (highest understood) |
| `"schema": 99` | ignored — above what this build reads |
| `"schema": 1.5` | ignored — must be integral, not merely numeric |
| `"schema": 0` | ignored — the valid range is `1..OTA_SCHEMA_MAX` |
| `"schema": "1"` | ignored — must be a JSON number |
| no readable block | `no_schema` |
| two blocks with the same schema | first wins, and a warning is logged |

Unknown keys **inside** a selected block are ignored, so extra metadata of your
own is safe to carry.

---

## Resolution: `devices` first, then `default`

```json
{
  "schema": 1,
  "default": { "version": "1.5.1", "url": "https://…/magtag_timer-1.5.1.bin" },
  "devices": {
    "magtag-a1b2c3": { "version": "1.6.0-rc1", "url": "https://…/…-1.6.0-rc1.bin" },
    "magtag-d4e5f6": { "version": null }
  }
}
```

The device looks itself up in `devices` by its **device id**. If it finds an
**object** there, that entry is **binding** — every subsequent rejection returns
an error rather than falling back to `default`. Installing the fleet version
because a device's own entry had a typo would ship exactly the build the
publisher meant to keep off that device, so not updating is the safe failure.
The one exception is an entry that is not an object at all (a string, a number):
that cannot express a targeting decision, so `default` applies.

With no `devices` match, `default` is used. With neither, the result is
`no_entry`.

### Finding a device's id

`magtag-` plus the last three bytes of the WiFi STA MAC, e.g. `magtag-a1b2c3`.
It is the middle segment of every MQTT topic the device publishes — subscribe to
`magtag/#` and read them off, or check the Home Assistant device page.

---

## The entry fields

### `version` — required, and it is what decides everything

- **Absent, or `null`** → the device is **pinned**. This is how you freeze a
  device without deleting its entry, so it is reported as `pinned`, not as an
  error.
- Must otherwise be a **non-empty string** shorter than 32 characters (the
  width of `esp_app_desc_t.version`). Anything else is `bad_version`.
- Compared against the running version for **difference, not newness**. A
  *lower* version is a valid instruction and will be installed — that is how a
  rollback gets published, and making this a greater-than test would mean lying
  about version numbers to recover a fleet.
- Equal to the running version → `up_to_date`, and no second window opens.

The string must match the running version **exactly**. That value comes from
`version.txt` (currently `1.5.0`) via `esp_app_get_description()->version`, and
it is what the panel and the Home Assistant firmware sensor show.

### `url` — the image, checked only once the version differs

- Must be a **string** beginning `https://` (case-insensitive) with a host after
  the scheme. Plain `http://` is rejected at every entry point: an
  unauthenticated firmware endpoint is an arbitrary-code-execution channel.
- Shorter than 192 characters, and free of spaces, control characters, `"` and
  `\`.
- Anything else is `bad_url`.

It is validated *after* the version comparison on purpose, so a device already
running the published version never reports a URL problem it would never act on.

---

## The retry budget

A failed download increments `ota_fails`, counted **against that exact target
version**. After `CONFIG_MAGTAG_OTA_MAX_FAILS` attempts (default 3) the device
reports `gave_up` and stops trying.

The escape is publishing a **different** version: the counter is keyed on the
target, so a new version re-arms the device with no reset needed from anywhere.
Re-publishing the *same* version will not.

---

## Things that will bite

**The clock must be set.** TLS cannot validate a certificate without a valid
time, so a device that has not reached NTP this session reports `no_time` and
skips the check rather than failing it.

**The CA is pinned into the image.** `main/certs/ota_ca.pem` is embedded at
build time, so the host serving both the manifest and the `.bin` must present a
chain up to that root, and the server certificate's CN/SAN must match the
hostname in the URL. A cert the device does not trust reports `tls_cert` — a
distinct code from `tls`, because it is a completely different fix.

**Rotating the CA is a one-way door.** The last update served under the *old*
root is the one that must carry the new root. Update every device before the
server stops serving a chain the old firmware trusts, or recovery needs a serial
cable.

**Redirects are not a free hop.** A `302` from `https://` to `http://` would
silently undo the whole guarantee, so redirect targets are re-validated against
the same `https://` rule (plan task 11).
