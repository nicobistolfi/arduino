# Raspberry Pi 5 setup

Reproducible log of how the Pi 5 was set up. Update as we go.

## Hardware

- Raspberry Pi 5
- 16GB microSD card (to be upgraded to a larger card later, see "Backup / migrate")
- Official 27W (5V/5A) USB-C power supply
- Host machine: Mac (Apple Silicon, built-in SD card reader)

## 1. Flash the OS

Tool: Raspberry Pi Imager.

```sh
brew install --cask raspberry-pi-imager
```

1. Insert the card into the Mac and confirm it shows up:
   ```sh
   diskutil list external physical
   ```
   (If nothing shows, re-seat the card or try a USB card reader.)

   Note: the Mac's built-in SD reader is classed as "internal", so the
   `external` filter shows nothing. Use `diskutil list` or
   `system_profiler SPCardReaderDataType` and look for the 15.6 GB removable
   disk instead. A new card with no partition table triggers macOS's "disk not
   readable" dialog; click **Ignore**. No formatting is needed, since Imager
   overwrites the whole card.
2. Open Raspberry Pi Imager and choose:
   - Device: **Raspberry Pi 5**
   - OS: **Raspberry Pi OS (64-bit)** (the full desktop image, not Lite)
   - Storage: the SD card (check the size before writing)
3. Click Next, then **Edit Settings**:
   - Hostname: `santa` (reachable as `santa.local`)
   - Username / password: `TODO`
   - Wi-Fi SSID / password / country: `TODO`
   - Services: enable SSH
4. Write the card.

## 2. First boot

1. Put the card in the Pi, connect Ethernet/monitor if used, plug in power.
2. Wait 1-2 minutes for the first boot to finish.
3. Connect:
   ```sh
   ssh <user>@santa.local
   ```
   Or use the desktop directly with a monitor and keyboard attached.

## 3. Update

```sh
sudo apt update && sudo apt full-upgrade -y
sudo reboot
```

## 4. Configuration

Record every change made on the Pi here (raspi-config options, packages installed, files edited in `/etc`), so the setup can be rebuilt.

- TODO

## Backup / migrate to a larger SD card

**Full image (on the Mac, with the Pi shut down and the card in the reader):**

```sh
diskutil list                          # find the card, e.g. /dev/disk4
diskutil unmountDisk /dev/disk4
sudo dd if=/dev/rdisk4 of=~/pi-backup.img bs=4m status=progress
gzip ~/pi-backup.img
```

Restore to the new card with Raspberry Pi Imager ("Use custom"), or:

```sh
gunzip -c ~/pi-backup.img.gz | sudo dd of=/dev/rdisk4 bs=4m
```

Then boot the Pi and expand the filesystem:

```sh
sudo raspi-config nonint do_expand_rootfs && sudo reboot
```

**Work only:** keep code in git, and rsync everything else:

```sh
rsync -avz <user>@<hostname>.local:~/projects/ ~/pi-backup/projects/
apt list --installed > packages.txt
```

## Log

- 2026-11-10: Started. Card not yet detected by the Mac (built-in reader showed no card).
- 2026-11-12: Card detected as `/dev/disk4` (15.6 GB SDHC, no partition table, "not readable" dialog from macOS is expected). Raspberry Pi Imager installed. Decisions: desktop OS, hostname `santa`.
