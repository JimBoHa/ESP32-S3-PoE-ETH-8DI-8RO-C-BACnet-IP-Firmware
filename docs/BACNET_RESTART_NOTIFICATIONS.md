# BACnet restart notifications — 1.1.4 development candidate

This candidate adds the BACnet Device Restart Procedure so a supervisory
engine can detect a short controller restart even when its ordinary offline
polling misses the outage. It retains the 1.1.2
[COV delivery recovery](BACNET_COMMAND_RECOVERY.md#confirmed-cov-delivery-recovery-112-development-candidate)
and [relay-driver recovery](RELAY_OUTPUT_RECOVERY.md) changes.

Initial 1.1.3 field tests decoded valid broadcast and observer-unicast restart
notifications, but no automatic supervisory output commands followed during
the observation windows. Fresh-demand manual reissues restored the outputs.
Automatic command restoration therefore has not passed acceptance. A restart
notification, a renewed subscription, a new output command, and verified
relay-driver state are separate results. Site captures and installation
details remain in private deployment records, outside the repository.
This document is not deployment authority or a BACnet certification claim.
The 1.1.4 candidate adds bounded startup announcements and a network-clock
restart timestamp; these changes do not establish a successful field result.

## Implemented behavior

The protocol core is [bacnet_restart.c](../main/bacnet_restart.c), with
application integration in [bacnet_app.c](../main/bacnet_app.c). Operations
are serialized by the existing BACnet object mutex.

After application startup completes, IPv4 and the BACnet socket are ready,
the Device instance is settled, and current inputs have been sampled, the
controller starts its I-Am campaign. The restart notification additionally
waits up to five seconds for valid clock information, without blocking normal
BACnet reads, writes, subscriptions, or I-Am. It then sends an
UnconfirmedCOVNotification to each configured recipient.
The notification contains:

- subscriber process identifier `0` and time remaining `0`;
- this controller's Device identifier as both initiating and monitored object;
- exactly `System_Status`, `Time_Of_Device_Restart`, and `Last_Restart_Reason`;
- the same frozen restart timestamp returned by the Device property, with
  neither property array indexes nor command priorities in the value list.

This is the procedure specified by
[ASHRAE 135-2012 Addendum ai, Clause 19.3](https://bacnet.org/wp-content/uploads/sites/4/2022/08/Add-135-2012ai.pdf).
I-Am alone is not a restart notification. Initial I-Am and restart notification
use the same completed-startup, settled-instance, and open-socket readiness
conditions; I-Am does not wait for NTP. Its local-broadcast sender returns the
actual UDP result to a scheduler with at most five attempts, at least one
second apart. Positive transport acceptance ends the campaign. Link return
starts another bounded identity-announcement campaign, not a new restart.
Provisional automatic-instance conflict discovery remains separate and never
allows point operations under an unsettled identity. Neither I-Am nor this
unconfirmed notification has a BACnet acknowledgment.

Each recipient has at most five resolution/send attempts, spaced by at least
1,000 ms while the task is ready to run. A positive transport result completes
that recipient; it does not prove receipt. Exhaustion is recorded, not retried
indefinitely. These retry limits are implementation policy, not an ASHRAE
delivery-time guarantee.

A network interruption does not create another restart event. Outstanding
attempts can resume when connectivity returns. Once the boot's recipient
snapshot has been taken, removing a recipient cancels its pending attempt;
adding a recipient does not manufacture a new restart notification. Added
recipients participate in the next actual boot.

## Recipient configuration and persistence

`Restart_Notification_Recipients` is a readable, whole-list writable Device
property. This is a BACnetLIST, not an array; array indexes, including index
zero, are rejected. AddListElement, RemoveListElement, and ReadRange support
for this property are not added. At most eight recipients and 256 encoded
payload bytes are accepted.

| Recipient choice | Supported behavior |
| --- | --- |
| Device identifier | Valid Device object instance; cached address binding or instance-limited local Who-Is discovery. The peer need not be online when the list is written. |
| Address, network `0`, empty MAC | Local BACnet/IP broadcast; no global destination network is inserted. |
| Address, network `0`, six-byte MAC | Direct IPv4 address and UDP port. |
| Direct address with another network number | Rejected, including direct global broadcast. No router-discovery implementation is claimed. |

A Device-ID binding can contain an IPv4 next-hop router and remote DNET/DADR.
That complete destination is retained when framing the notification. Discovery
is local, however; acquisition of a remote binding is not guaranteed. Missing
or invalid bindings produce bounded resolution failures, never an implicit
broadcast. Malformed, oversized, or unsupported list writes leave the old
live list unchanged.

An absent stored property selects one local-broadcast recipient. A valid
empty list deliberately disables notifications. Corrupt or unreadable stored
configuration suppresses notifications and records an error; corruption is
not permission to broaden recipients. These default and empty-list semantics
follow [ASHRAE 135-2010 Addendum af, Clause 12.11.46](https://bacnet.org/wp-content/uploads/sites/4/2022/08/Add-135-2010af.pdf).
The standard permits a read-only recipient property only when it contains
the default; this implementation instead supports configuration.

[config_store.c](../main/config_store.c) stores the encoded list under the
separate `bacnet_cfg/restart_rcpt` NVS key, with a version byte and two-byte
payload length. An explicit empty list therefore differs from a missing key.
This does not enlarge or migrate the existing 1,052-byte configuration blob,
change the Device identity, or rewrite the admin key, relay mask, or priorities.
Successful persistence precedes replacement of the live list and eligibility
for a WriteProperty SimpleACK. Identical payloads avoid another flash commit.

Persistence is an implementation requirement here. The published
[ASHRAE restart test, Addendum 135.1-2009g, printed pp60–61](https://bacnet.org/wp-content/uploads/sites/4/2022/08/Add-135-1-2009g.pdf)
configures recipients, resets the device, and checks delivery and matching
restart properties. The recipient-property clause itself does not provide
an explicit nonvolatile-storage implementation rule.

## Clock and ownership limits

The 1.1.4 [network clock](NETWORK_TIME.md) supplies validated NTP time or
validated warm-retained NTP holdover. The application samples it under the
BACnet object mutex, normally at most once per second and once again at the
restart-selection deadline, before handling incoming BACnet requests. Valid
local date/time seeds the pinned advancing millisecond clock;
the Device's four local-time properties report the configured timezone's
local DateTime, BACnet standard-time UTC offset (minutes west), and actual DST
status. NTP callbacks never call the BACnet stack. RTC presence detection
alone does not establish a valid wall clock. BACnet TimeSynchronization and
UTCTimeSynchronization services remain unimplemented; NTP is separate.

At the first completed-startup/network-ready episode, timestamp selection
starts a nonblocking five-second clock wait. If a valid snapshot is available,
its boot-local DateTime (wall-clock anchor minus uptime) is frozen. If no valid
clock is available by the deadline, exactly `1990-01-01 00:00:00.00` is frozen
as fallback. Disconnected time can advance that deadline, but no timestamp is
selected or notification sent while readiness is false. Prior to selection,
Time_Of_Device_Restart exposes a provisional fallback and management reports
`timestamp_frozen=false`/`pending`; notification delivery remains deferred.

The selected timestamp is copied into both Device RP state and the restart
encoder before any recipient is attempted. Every recipient and send retry
uses the same value. Late NTP correction, timezone edits, recipient edits,
and link return cannot alter that boot's frozen timestamp or fabricate a new
restart event. Current Local_Date/Local_Time can still synchronize afterward.

The fallback is deliberately **not actual wall-clock time** and can repeat
across boots. Do not infer elapsed outage time or unique boot identity from
it; use uptime/reboot count and an external capture clock. DateTime form is
required because this Device exposes Local_Date and Local_Time. The specified
no-clock startup fallback and timestamp rules are in
[ASHRAE 135-2008 Addendum ac, Clauses 12.1 and 12.11.22–23](https://bacnet.org/wp-content/uploads/sites/4/2022/08/Add-135-2008ac.pdf).
The core rejects invalid or wildcard restart timestamps rather than sending
an uninitialized value.

This feature does not write points on another device, retain or replay BACnet
priorities, enable relay-state restoration, clear another writer, or force
Metasys logic to run. Boot output policy and command ownership remain unchanged.

## Diagnostics and local verification

The read-only management status includes `bacnet_restart`, or `null` if its
mutex-protected snapshot is unavailable. Fields are `boot_ready`,
`recipients_count`, `pending_recipients`, `notifications_sent`, `send_failures`,
`resolution_failures`, `exhausted_recipients`, `configuration_errors`, and
`persistence_failures`. Timestamp diagnostics distinguish pending selection,
valid network-clock/holdover selection, and `unsynchronized-1990-fallback`;
they include the frozen DateTime and selection/wait-start uptime. Per-recipient
diagnostics retain this boot's recipient snapshot, resolution status and actual
resolved BACnet address, attempt/send counts, last transport result and times,
and cancellation status. This snapshot can differ from the subsequently edited
configured list. Counters are per boot. In particular, `notifications_sent`
means transport acceptance, not NAE processing.

`bacnet_announcement` separately exposes readiness, pending/exhausted state,
ready-episode and attempt counts, transport acceptances/failures, the last
result, and first/last/accepted/next-attempt uptime. These diagnostics describe
scheduled startup/link-return I-Am, not every Who-Is response or provisional
instance-discovery announcement. [Network time](NETWORK_TIME.md) documents
current clock quality independently of the frozen boot timestamp.

Using the supported Python and compiler described in
[Development](DEVELOPMENT.md), the focused local tests are:

```sh
python3 tests/run_bacnet_restart_tests.py
python3 tests/run_restart_store_tests.py
python3 tests/run_bacnet_announcement_tests.py
```

The core harness uses pinned BACnet codecs and ASan/UBSan to check wire
decoding, recipient validation, atomic writes, timestamps, bounded retries,
recipient edits, deferred timestamp selection, unchanged recipient storage,
immutable timestamps across retries/link return, destination diagnostics, and
malformed input. The pure startup model tests readiness gating, actual
send-result backoff/exhaustion, successful-send suppression, bounded clock
selection, and late synchronization. The storage harness compiles the real
configuration store against isolated NVS/mutex stubs, including error and
preservation scenarios. Neither opens a device socket or actuates a relay.
These are included in `tests/run_host_tests.sh`. Record actual test and ESP-IDF
build results separately; the existence of these tests is not a passing
field result.

## Safe field acceptance

Use one coordinator and the current operator-authorized maintenance window.
Do not run disconnected-load HIL, flash erasure, NVS fault injection, broad
discovery stress, or unapproved point writes on connected doors. Keep site
addresses, keys, captures, and image hashes in private deployment records.

1. Before any authorized restart, save image/configuration identity, recipient
   list, actual source state and current LCT demand, every affected remote
   priority slot, and relay-driver health/registers. Confirm intended unlock
   duration and a bounded cleanup plan. Preserve other writers' slots.
2. Capture the restart notification after readiness. Decode every required
   field and actual destination; compare timestamp/reason against fresh Device
   reads. Confirm no premature notification or unintended broadcast scope.
   Record I-Am separately and distinguish a valid NTP boot timestamp from
   holdover or fallback. A subsequent time/zone correction must not produce
   another restart notification or rewrite the already announced timestamp.
3. Separately observe whether the NAE renews subscriptions. A subscription or
   updated mapped Present_Value is not proof of a command. JCI documents
   delayed resubscription after an undetected third-party restart; it does not
   promise LCT output reassertion in response to this notification. The
   [JCI integration troubleshooting guide, Release 11](https://docs.johnsoncontrols.com/bas/r/Metasys/en-US/BACnet-Controller-Integration-Technical-Bulletin/11.0/Troubleshooting/BACnet-System-Integration-troubleshooting-guide)
   is supporting guidance, not an exact NAE 9.0.5 compatibility guarantee.
4. Keep the demand unchanged across the restart. Require a fresh NAE
   WriteProperty for the correct remote BO, value, and intended priority,
   followed by the corresponding remote priority state, effective value, and
   successful driver verification. Do not issue a manual command or toggle
   logic to make this automatic-reassertion check pass. A proposed site target
   is within 10 seconds of BACnet readiness; record notification,
   resubscription, command, and driver times independently. This target is not
   guaranteed by the BACnet standard.
5. Where an authorized real input transition is available, measure its
   separate end-to-end command path. Aim for less than five seconds; five to
   ten seconds is the stated acceptable range. An output-only override cannot
   validate input-to-LCT timing. Without a person or independent feedback at
   the door, report physical movement/contact timing as unverified.
6. If commands do not reappear, report notification/subscription success and
   command-reassertion failure separately. Do not hide the failure by replaying
   a saved relay mask. End the test using only the authorized cleanup scope,
   then verify fresh source, remote priorities, and driver state.

Mapped Metasys priority-array values may lag the remote device independently
of mapped Present_Value. Use fresh remote reads and received-command evidence
for arbitration decisions. Driver CONFIG/OUTPUT readback verifies the expander
path, not relay contacts or door position. Successful local tests and OTA
installation alone do not satisfy these field acceptance criteria.
