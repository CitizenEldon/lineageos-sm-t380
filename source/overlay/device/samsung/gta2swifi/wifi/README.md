Reviewed Wi-Fi integration for the full Lineage23.2/Android16 product.

Uses immutable diagnostic020 and its exact WLAN module, byte-identical to the
reviewed/tested017–019 WLAN ABI. No identity or credentials are packaged.
The payload manifest pins18 firmware/regulatory/module files; payloads remain
outside Git and are preserved with source backups and immutable diagnostics.

EFS is ro/noload; only the fixed factory MAC is copied to volatile RAM.
The helper starts asynchronously, waits for actual NV/CBC readiness, inserts
the module and checks wlan0 identity before publishing prepared success.
The stock Wi-Fi HAL has no init interface autostart mapping before that gate.
FT/LFR/ESE are0; PMF stays enabled and the default regulatory country isUS.
No P2P/AP feature is declared. Full policy/helper/init and runtime validation
are recorded in build/artifact receipts, not inferred from source review.

Review: Windows docs/WIFI-INTEGRATION-REVIEW.md and android/wifi-next/REVIEW.md.
