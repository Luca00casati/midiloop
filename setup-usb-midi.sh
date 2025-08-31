#!/bin/bash
# Setup udev rule for USB MIDI interface (fc02:0101) for current user

USER_NAME=$(whoami)
RULES_FILE="/etc/udev/rules.d/50-usb-midi.rules"

echo "Creating udev rule for USB MIDI device (fc02:0101) owned by $USER_NAME..."

# Write the rule
RULE="SUBSYSTEM==\"usb\", ATTR{idVendor}==\"fc02\", ATTR{idProduct}==\"0101\", MODE=\"0660\", OWNER=\"$USER_NAME\""

# Use sudo to write the file
echo "$RULE" | sudo tee "$RULES_FILE" > /dev/null

# Reload rules
echo "Reloading udev rules..."
sudo udevadm control --reload-rules
sudo udevadm trigger

echo "Done. Please unplug and replug your USB MIDI interface."

