# ESP32-C6 Firmware

`network_adapter_2.12.3.bin` – offizielle ESP-Hosted-Slave-Firmware 2.12.3 für den ESP32-C6,
aus Elecrows Paket `C6_Upgrade_and_IDF_Source_Code_Package.zip`
(Repo CrowPanel-Advanced-9inch-…, `example/CrowPanel_Advance_ESP32-P4 Display_Firmware_Upgrade_Guide/`).

SHA256: `62cbea4ebdfbfea6bce961a519185ac6ff55080d393cab5653485d64822b7d86` (stimmt mit Elecrows SHA256SUMS überein)

Wird nur eingebaut, wenn `CONFIG_LOXPANEL_C6_UPDATE=y`. Die Host-Seite (`esp_hosted` in
`components/network/idf_component.yml`) muss dieselbe Version haben.
