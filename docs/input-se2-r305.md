# SE 2 frontend and native touch

R305 applies authentic D79AP/iPhone12,8 product data from Apple's17.7/21H16
DeviceTree to the retained E5 ADT. Product IDc459fdb0eef455588dbc6f587c20a5372792bbf8
matches this21H461 MobileGestalt table at row57/type0x80bb21e1. Phone artwork,
subtype569 and native fallback are retained; artwork scale and display-scale
are1 for the physical320x480 panel. Hardware compatibility suffixes, MMIO,
root UUID/reservations and all unrelated ADT property bytes are unchanged.
The ADT fits its prior allocation; the kernel/HID ABI remains unchanged.

All automatic input is disabled. The operator confirms suitable layout,
manual Home, app switcher and touch.601 callbacks authorize599 valid reads,
with2 coalesced events,594 event-work/5 timer reads and zero errors/recoveries.
294 hardware changes include23 stationary single-contact releases,6 drags
and2 dual releases; all-up is complete and final active is zero.426HID reports
have no failure/drop. Original event processing/client delivery pass.
2433 display frames and30 original-checksum captures pass.900s window,
full payload readback, three37/37 windows and Android A return pass.

Settings content remains black. Home after that state was not tested.
Current UUIDB2D258C1-4B16-4C62-8472-73A1959311C2 is new in post-run timesync,
but its persisted UI logs do not cover the late Settings action. Historical
UUID610F653D already exists beforeR305, so its Preferences launch cannot
establish this run's behavior. No unique rendering cause is claimed.
See [runtime evidence](logs/input-se2-r305.json) and the e5-xnu R305 archive.
