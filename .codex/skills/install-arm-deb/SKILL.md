---
name: install-arm-deb
description: Install a specified UGripper arm64 deb package onto an RK3588/ARM target over SSH using the repository install script; use when the user asks to install, deploy, or push a deb to 3588ether or another arm board.
---

# install-arm-deb

Use this skill when a user asks to install a specific `.deb` onto `3588ether` or another ARM target.

## Workflow

1. Resolve the deb path the user requested. If they only gave a version, prefer `ugripper_<version>_arm64.deb` in the repository root.
2. Run the repository script:
   ```bash
   TARGET_PASSWORD='<password>' \
   bash scripts/install_deb_to_arm_target.sh \
     --deb <deb-path> \
     --host <target-host> \
     --user <target-user>
   ```
3. Defaults for `3588ether` / current RK3588 target:
   - host: `192.168.1.110`
   - user: `ubuntu`
   - password: `ubuntu`
   - command should normally pass `TARGET_PASSWORD='ubuntu'`
4. Report the installed package version from script output and whether `ugripper.service` is active, failed, or only partially shown.

## Notes

- The script copies the deb to `/tmp/ugripper-deb-install/` on the target, then runs `sudo dpkg -i`.
- Do not manually expand this into ad hoc `ssh` / `scp` commands unless the script itself fails and needs debugging.
