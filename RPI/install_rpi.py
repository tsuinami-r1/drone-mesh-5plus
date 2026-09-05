#!/usr/bin/env python3
"""
mesh-mapper Raspberry Pi installer for tsuinami-r1/drone-mesh-5plus

Downloads mesh-mapper.py and requirements.txt from this repository, installs
the Python dependencies, and (optionally) adds an @reboot cron job so the
collection point comes up on its own after a power cycle.

Usage:
    python3 install_rpi.py                          # main branch, ~/mesh-mapper, cron on
    python3 install_rpi.py --branch <name>          # any branch of the repo
    python3 install_rpi.py --install-dir /opt/mesh-mapper
    python3 install_rpi.py --no-cron --skip-deps --force

Only the mapper is installed here. Firmware is built and flashed from a
workstation with PlatformIO; see the repository README.
"""

import os
import sys
import argparse
import subprocess
import getpass
from pathlib import Path

try:
    import requests
except ImportError:  # pragma: no cover - bootstrap path on a bare Pi
    print("❌ The 'requests' module is required to run this installer.")
    print("   sudo apt install -y python3-requests    (or: pip3 install requests)")
    sys.exit(1)

# GitHub repository configuration — this fork, not upstream.
GITHUB_REPO = "tsuinami-r1/drone-mesh-5plus"
GITHUB_BASE_URL = f"https://raw.githubusercontent.com/{GITHUB_REPO}"
TARGET_FILE = "mesh-mapper.py"
REQUIREMENTS_FILE = "requirements.txt"
FILES_TO_INSTALL = (TARGET_FILE, REQUIREMENTS_FILE)

# Fallback if requirements.txt cannot be fetched; keep in sync with the repo.
FALLBACK_REQUIREMENTS = [
    "flask>=2.0.0",
    "flask-socketio>=5.0.0",
    "requests>=2.25.0",
    "urllib3>=1.26.0",
    "pyserial>=3.5",
]


def get_current_user():
    """Get the current username"""
    return getpass.getuser()


def get_user_home():
    """Get the current user's home directory"""
    return str(Path.home())


def construct_download_url(branch, filename):
    """Construct the raw GitHub URL for downloading a file"""
    return f"{GITHUB_BASE_URL}/{branch}/{filename}"


def download_file(url, destination, executable=False):
    """Download a file from URL to destination"""
    print(f"📥 Downloading from: {url}")
    print(f"📁 Saving to: {destination}")

    try:
        response = requests.get(url, stream=True, timeout=30)
        response.raise_for_status()

        os.makedirs(os.path.dirname(destination), exist_ok=True)

        with open(destination, 'wb') as f:
            for chunk in response.iter_content(chunk_size=8192):
                if chunk:
                    f.write(chunk)

        if executable:
            os.chmod(destination, 0o755)

        print(f"✅ Downloaded successfully: {destination}")
        return True

    except requests.exceptions.RequestException as e:
        print(f"❌ Error downloading file: {e}")
        return False
    except Exception as e:
        print(f"❌ Error saving file: {e}")
        return False


def check_file_exists(url):
    """Check if file exists at the URL"""
    try:
        response = requests.head(url, timeout=10, allow_redirects=True)
        return response.status_code == 200
    except requests.exceptions.RequestException:
        return False


def install_dependencies(install_dir):
    """Install Python dependencies from requirements.txt.

    Raspberry Pi OS Bookworm and newer mark the system Python as externally
    managed (PEP 668), so a plain pip install is refused. Try that first for
    older images, then fall back to --break-system-packages, which is what a
    dedicated collection-point Pi wants anyway.
    """
    req_path = os.path.join(install_dir, REQUIREMENTS_FILE)
    if os.path.exists(req_path):
        target = ['-r', req_path]
        print(f"📦 Installing dependencies from {req_path}")
    else:
        target = list(FALLBACK_REQUIREMENTS)
        print("📦 requirements.txt missing, installing the built-in list")

    strategies = [
        ("pip install", [sys.executable, '-m', 'pip', 'install']),
        ("pip install --break-system-packages",
         [sys.executable, '-m', 'pip', 'install', '--break-system-packages']),
        ("pip install --user --break-system-packages",
         [sys.executable, '-m', 'pip', 'install', '--user', '--break-system-packages']),
    ]

    for name, base_cmd in strategies:
        print(f"🔧 Trying: {name}")
        result = subprocess.run(base_cmd + target, capture_output=True, text=True, check=False)
        if result.returncode == 0:
            print("✅ Dependencies installed")
            return True
        last_line = (result.stderr or result.stdout).strip().splitlines()
        if last_line:
            print(f"   {last_line[-1]}")

    print("❌ Could not install dependencies automatically.")
    print(f"   Try manually: {sys.executable} -m pip install --break-system-packages -r {req_path}")
    return False


def verify_imports():
    """Confirm the mapper's imports resolve in this interpreter"""
    test = "import flask, flask_socketio, requests, urllib3, serial, serial.tools.list_ports"
    result = subprocess.run([sys.executable, '-c', test], capture_output=True, text=True, check=False)
    if result.returncode == 0:
        print("✅ Python imports verified")
        return True
    print(f"⚠️  Import check failed: {result.stderr.strip().splitlines()[-1] if result.stderr else 'unknown'}")
    return False


def get_current_crontab():
    """Get current user's crontab"""
    try:
        result = subprocess.run(['crontab', '-l'],
                                capture_output=True, text=True, check=False)
        if result.returncode == 0:
            return result.stdout.strip()
        return ""  # No crontab exists
    except Exception as e:
        print(f"⚠️  Error reading crontab: {e}")
        return ""


def install_cron_job(install_dir):
    """Install the cron job for auto-start"""
    current_user = get_current_user()

    cron_command = (f"@reboot sleep 5 && cd {install_dir} && "
                    f"{sys.executable} {TARGET_FILE} --debug")

    print(f"🔧 Setting up cron job for user: {current_user}")
    print(f"📍 Install directory: {install_dir}")
    print(f"⚙️  Cron command: {cron_command}")

    current_crontab = get_current_crontab()

    # Replace any earlier entry for this install dir
    lines = [line for line in current_crontab.split('\n')
             if not (f"cd {install_dir}" in line and TARGET_FILE in line)]
    if len(lines) != len(current_crontab.split('\n')):
        print("ℹ️  Cron job already exists, updating...")
    lines = [line for line in lines if line.strip()]
    lines.append(cron_command)
    new_crontab = '\n'.join(lines) + '\n'

    try:
        process = subprocess.Popen(['crontab', '-'],
                                   stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE,
                                   text=True)
        _, stderr = process.communicate(new_crontab)

        if process.returncode == 0:
            print("✅ Cron job installed successfully!")
            print("🔄 mesh-mapper will auto-start on system reboot")
            return True
        print(f"❌ Error installing cron job: {stderr}")
        return False

    except Exception as e:
        print(f"❌ Error setting up cron job: {e}")
        return False


def verify_installation(install_path, cron_expected=True):
    """Verify the installation"""
    print("\n🔍 Verifying installation...")

    if not os.path.exists(install_path):
        print(f"❌ File not found: {install_path}")
        return False

    if not os.access(install_path, os.X_OK):
        print(f"⚠️  File is not executable: {install_path}")
        return False

    file_size = os.path.getsize(install_path)
    if file_size < 1000:
        print(f"⚠️  File seems too small ({file_size} bytes): {install_path}")
        return False

    print(f"✅ Installation verified: {install_path} ({file_size} bytes)")

    if cron_expected:
        current_crontab = get_current_crontab()
        if TARGET_FILE in current_crontab and "@reboot" in current_crontab:
            print("✅ Cron job verified")
            return True
        print("⚠️  Cron job not found in crontab")
        return False

    print("ℹ️  Cron job verification skipped (--no-cron used)")
    return True


def main():
    parser = argparse.ArgumentParser(
        description=f"Install mesh-mapper.py from github.com/{GITHUB_REPO} on a Raspberry Pi",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 install_rpi.py
  python3 install_rpi.py --branch main --install-dir /opt/mesh-mapper
  python3 install_rpi.py --no-cron
  python3 install_rpi.py --force --skip-deps
        """)

    parser.add_argument('--branch',
                        default='main',
                        help='Repository branch to download from (default: main)')

    parser.add_argument('--install-dir',
                        default=None,
                        help='Installation directory (default: ~/mesh-mapper)')

    parser.add_argument('--no-cron',
                        action='store_true',
                        help='Skip installing the @reboot cron job')

    parser.add_argument('--skip-deps',
                        action='store_true',
                        help='Do not pip-install requirements.txt')

    parser.add_argument('--force',
                        action='store_true',
                        help='Overwrite an existing installation without asking')

    args = parser.parse_args()
    branch = args.branch

    if args.install_dir:
        install_dir = os.path.abspath(args.install_dir)
    else:
        install_dir = os.path.join(get_user_home(), 'mesh-mapper')

    install_path = os.path.join(install_dir, TARGET_FILE)

    print("=" * 60)
    print("🚁 MESH-MAPPER RASPBERRY PI INSTALLER")
    print("=" * 60)
    print(f"📦 Repository: https://github.com/{GITHUB_REPO}")
    print(f"🌿 Branch: {branch}")
    print(f"👤 User: {get_current_user()}")
    print(f"📁 Install Dir: {install_dir}")
    print(f"📄 Files: {', '.join(FILES_TO_INSTALL)}")
    print()

    if os.path.exists(install_path) and not args.force:
        response = input(f"File already exists: {install_path}\nOverwrite? (y/N): ")
        if response.lower() != 'y':
            print("❌ Installation cancelled")
            return 1

    mapper_url = construct_download_url(branch, TARGET_FILE)
    print("🔍 Checking that the branch exists on GitHub...")
    if not check_file_exists(mapper_url):
        print(f"❌ File not found at: {mapper_url}")
        print(f"   Check that branch '{branch}' exists in {GITHUB_REPO} and contains {TARGET_FILE}")
        return 1
    print(f"✅ Found {branch}/{TARGET_FILE}")

    if not download_file(mapper_url, install_path, executable=True):
        print("❌ Download failed")
        return 1

    req_url = construct_download_url(branch, REQUIREMENTS_FILE)
    if not download_file(req_url, os.path.join(install_dir, REQUIREMENTS_FILE)):
        print("⚠️  requirements.txt not downloaded; the built-in dependency list will be used")

    if args.skip_deps:
        print("⏭️  Skipping dependency installation (--skip-deps specified)")
    else:
        print("\n🐍 Installing Python dependencies...")
        if install_dependencies(install_dir):
            verify_imports()
        else:
            print("⚠️  Dependency installation failed; the mapper will not start until they are installed")

    if not args.no_cron:
        print("\n🕒 Setting up auto-start cron job...")
        if not install_cron_job(install_dir):
            print("⚠️  Cron job installation failed, but the mapper was downloaded successfully")
    else:
        print("⏭️  Skipping cron job installation (--no-cron specified)")

    if verify_installation(install_path, not args.no_cron):
        print("\n🎉 Installation completed successfully!")
        print(f"📍 mesh-mapper installed at: {install_path}")

        if not args.no_cron:
            print("🔄 Auto-start enabled: will run on system reboot")
        print("💡 To run it now:")
        print(f"   cd {install_dir} && python3 {TARGET_FILE} --debug")

        print("\n📋 Next steps:")
        print("  1. Plug the home node (XIAO running home_node, wired to its Heltec) into the Pi's USB")
        print("  2. Run the mapper manually and pick the home node's serial port in Settings")
        print("  3. Open the map from any machine on the LAN: http://<pi-ip>:5000")
        print("  4. Reboot to confirm auto-start (if cron enabled); logs land in mapper.log")
        print(f"  5. To update later, re-run this script with --force (branch: {branch})")

        return 0

    print("❌ Installation verification failed")
    return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n⏹️  Installation cancelled by user")
        sys.exit(1)
    except Exception as e:
        print(f"\n💥 Unexpected error: {e}")
        sys.exit(1)
