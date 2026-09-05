# Hardware acceptance testing

The hardware-in-the-loop runner performs the repeatable, software-visible
portion of commissioning with an independent BACpypes3 client. It tests both
broadcast and directed discovery, the complete object map and metadata,
ReadPropertyMultiple `ALL`, Who-Has/I-Have, confirmed and unconfirmed COV,
the 16-subscription capacity bound and cleanup, and representative negative
access responses. By default it does not actuate outputs.

Create an isolated environment and install the pinned test dependency:

```sh
python3 -m venv .hil-venv
. .hil-venv/bin/activate
python -m pip install -r requirements-hil.txt
```

Run the non-actuating suite from a host address on the controller's subnet.
The local BACnet client binds UDP 47808, so stop other BACnet clients using
that address and port first:

```sh
python tools/bacnet_hil_test.py \
  --local-address 192.168.75.191/24 \
  --device-address 192.168.75.154 \
  --device-instance 599153 \
  --report hardware-report.json
```

To include physical relay commands, first disconnect every controlled load and
verify that no automation workstation is commanding the outputs. The runner
refuses to overwrite a nonempty priority array and always attempts to
relinquish any priority slots it touched:

```sh
python tools/bacnet_hil_test.py \
  --local-address 192.168.75.191/24 \
  --device-address 192.168.75.154 \
  --device-instance 599153 \
  --exercise-relays \
  --relay-safety-confirmation loads-disconnected \
  --relay-seconds 3 \
  --report hardware-relay-report.json
```

This runner validates BACnet commands, firmware readback, priority behavior,
and COV. It cannot verify relay contact continuity or optocoupler electrical
behavior. Complete those checks with a meter and a protected input fixture.
Cold boot, brownout, rapid power cycling, failed-startup OTA rollback, and
corrupt-NVS recovery also require USB recovery access and remain separate
field tests.
