# BACnet command recovery after an update or restart

A retained device/point mapping and a retained output command are different
things. A BAS can rediscover the same controller, read its points, and renew
COV subscriptions while sending no new output command. COV reports values;
it does not request that an output remain on.

## What survives

The following applies to compatible Ethernet OTA updates and normal reboots,
with intact NVS. Initial USB erase/installation is different: it removes
configuration, identity locks, keys, and saved runtime state.

| State | Behavior |
|---|---|
| Commissioned Device instance | A locked v1.1.0 instance stays fixed; manual assignments also stay fixed. See the legacy upgrade exception in [automatic assignment](AUTOMATIC_INSTANCE.md#existing-firmware-settings-and-api). |
| Object identifiers and configured names | BI1–8 and BO1–8 keep their identifiers; configured names remain in NVS. |
| Network settings and admin key | Retained by compatible OTA. DHCP may supply a different address; use a site-managed reservation where a stable IP is required. |
| BAS point mapping/control program | Stored in the BAS, not in the ESP32. Verify its references and back up its current database. |
| BACnet priority arrays and COV subscriptions | Volatile. Clients must renew subscriptions and reassert current demand as required. |
| Effective relay mask | Off by default at startup. Optional relay restore uses the last saved mask as the new boot's Relinquish_Default, not as restored priority slots. |

Do not request a new automatic instance, erase flash, or delete/reimport BAS
points merely because an output is off. First establish whether identity or
the command is actually missing.

## Read-only diagnosis

Read management state without the admin key:

```sh
python tools/device_admin.py --device DEVICE_IP status
python tools/device_admin.py --device DEVICE_IP config-get
```

Record the active instance and lock status, firmware, uptime/reboot count,
relay masks, active priorities, relay-controller health, and
`restore_relay_state`. Retain the snapshot privately, not in the repository.

Use a BACnet client to read each affected remote BO's `Present_Value`, all
16 `Priority_Array` entries, `Relinquish_Default`, `Reliability`, and
`Out_Of_Service`. Also inspect the BAS's mapped copy and its remote device,
object type, and instance references. A mapped BO's own identifier can differ
from the remote BO identifier.

| Observation | Interpretation / next check |
|---|---|
| Remote priority array all NULL, default inactive, Present_Value inactive | No retained command is currently present. Check the demand source and whether it reissues commands after a remote restart. |
| Active demand at one priority, inactive at a higher priority | Resolve the owning control/override logic. Lower priority numbers win; do not clear another owner's command blindly. |
| BAS mapped value or reference differs from the remote point | Check binding, stale data, integration health, and mapping configuration. |
| Fresh COV subscription from a supervisory engine | The engine is subscribing to the point; this does not prove it is issuing WriteProperty commands. |
| Commanded mask active but relay-controller unhealthy | Investigate the expander/I2C path with loads isolated. |
| Command and successful expander-write telemetry agree | Software paths agree. This board has no contact-feedback input; a meter or independent feedback is still required. |

Compare both devices' restart times. A remote ESP32 restart need not restart
the supervisory engine, and a short outage may not trigger its offline/online
logic. A current empty array is not a historical packet trace: commands may
have expired or been relinquished before inspection.

## Metasys-specific checks

The ADS/ADX server address may differ from the NAE that owns the integration.
Find the mapped object's actual supervisory engine and control involvement.
Inspect its current schedule/program demand, enable state, Local Control,
override/temporary-override state, selected write priority, and remote
reference in the version-appropriate SMP or Metasys interface.

Do not use a one-time or expiring operator command as evidence of a persistent
control link. Johnson Controls documents Temporary Override as timed and
automatically released. Its BO state commands can also release priorities
9–15 before writing priority 16, so an operator On command can disturb other
control strategies. See [Metasys BO commands](https://docs.johnsoncontrols.com/bas/r/Metasys/en-US/Metasys-Site-Management-Portal-Help/11.0/Insert-Menu/Field-Point/Binary-Output-Object/Binary-Output-Commands).

Inspect Auto Restore and Restore Command Priority where the installed engine
and object type support them. Do not infer that a setting is false from an
unsupported BACnet property read. The newer
[Options tab reference](https://docs.johnsoncontrols.com/bas/r/Metasys/en-US/SCT-System-Configuration-Tool-Help/19.0/Welcome/Object-Help/Object-and-Feature-Tabs/Options-Tab)
lists those settings, but its defaults are not a measurement of an older
installed engine. Johnson Controls' [restart-option definition](https://docs.johnsoncontrols.com/bas/r/Metasys/en-US/SCT-System-Configuration-Tool-Help/19.0/Insert-Menu/Field-Point/Binary-Value-Object/Binary-Value-Attributes)
describes saving selected commands at shutdown and restoring them at startup.
Do not infer from that definition alone that an engine will reassert demand
when only a third-party field device reboots; verify the installed integration.

Have the controls owner verify a version-supported strategy that reasserts
the current, interlocked demand after a remote restart or reconnection. If a
periodic refresh is appropriate, it must preserve command ownership and use
a reviewed interval; do not continuously force On from a separate script.
Verify demand removal/relinquish as well as demand assertion, then upload the
tested BAS configuration into its recovery archive.

Audit records can establish a logged operator command and its expiry, but
absence from an audit repository does not prove that no automation or direct
BACnet writes occurred. An unavailable Metasys UI/API is an access problem to
investigate separately; it does not by itself prove that a still-running NAE
has stopped controlling its field devices.

## Optional local relay restoration

**Safety warning:** Do not enable automatic relay restoration on connected
equipment until the controls owner has approved automatic re-energization and
reviewed the required interlocks. An output can energize before the BAS
reconnects. This is not a replacement for an engineered safe-state strategy.

`restore_relay_state` is disabled by default and takes effect after a reboot.
When enabled, successfully written output-mask changes are saved to NVS after
five stable seconds. At boot, firmware clears the expander outputs first,
then applies the saved mask and sets each BO's Relinquish_Default accordingly.

The limitations matter:

- Priority values, writer identity, override expiry, schedules, and interlocks
  are not saved. The priority arrays still start empty.
- A saved On becomes the boot's default. Relinquishing a later command can
  therefore return to On, even when the original demand has expired.
- An abrupt outage before a successful delayed save can restore an older mask.
- Disabling restore does not erase the saved mask. Re-enabling it can revive
  an old state, even when outputs are currently off. Current telemetry does
  not expose the saved NVS mask.
- Outputs have an off interval during initialization. This cannot provide
  uninterrupted operation through a reboot or power outage.
- Stable state changes cause NVS writes; do not treat it as a fast-cycle logger.

For a disconnected-load bench acceptance test:

1. Record the existing configuration and all priority arrays. Isolate every
   load, coordinate with the BAS owner, and preserve local recovery access.
2. If local restoration is intended, save that setting and reboot while
   loads remain disconnected; a stale saved mask may appear at this step.
3. Apply the desired test demand through its intended BAS owner. Verify the
   remote priority/value and expander-write telemetry, then allow more than
   five stable seconds and check for save failures in local diagnostics.
4. Reboot or install the approved compatible OTA image. Record the same
   instance/mappings, actual startup state, restored default, renewed COV,
   and any new command with its priority. Distinguish local restoration from
   an actual BAS reassertion.
5. Verify that the BAS can remove demand and that relinquishing behaves as
   intended. Check contacts independently before reconnecting any load.
6. If abandoning restore, first establish and save an all-off mask while
   restore is still active. Then disable it, reboot, verify all arrays and
   masks are clear, and restore the agreed BAS operating policy. Do not leave
   temporary test overrides in service.

Cold power-cycle and failed-startup rollback tests require physical access and
a recovery plan. Record them separately from an authenticated warm reboot.
