# Node.js PC/SC Smart Card Addon (`nfc-reader-nodejs`)

[![NPM version](https://img.shields.io/npm/v/nfc-reader-nodejs.svg)](https://www.npmjs.com/package/nfc-reader-nodejs)
[![Build Status](https://img.shields.io/travis/your-username/your-repo-name.svg)](https://travis-ci.org/your-username/your-repo-name) <!-- Replace with your CI link -->
[![License](https://img.shields.io/npm/l/nfc-reader-nodejs.svg)](LICENSE) <!-- Add a LICENSE file -->

A native Node.js addon for interacting with PC/SC (Personal Computer/Smart Card) compatible smart card readers on Windows, Linux, and macOS.

## Features

*   **List Readers:** Enumerate all connected PC/SC compliant smart card readers.
*   **Card Event Listener:** Listen for card insertion/removal events on a specific reader.
*   **Automatic UID Reading:** Automatically attempts to read the card's UID (using the standard `FF CA 00 00 00` APDU) upon insertion when listening.
*   **Transmit APDUs:** Send custom raw APDU (Application Protocol Data Unit) commands to the card and receive the raw response.
*   **Asynchronous Operations:** Core I/O operations (`transmit`, background listening) are performed asynchronously to avoid blocking the Node.js event loop.

## Prerequisites

This is a native addon, meaning it requires compilation on the target machine. Ensure you have the necessary build tools and PC/SC dependencies installed.

### 1. Node.js and npm

*   Node.js (LTS version recommended)
*   npm (usually comes with Node.js)

### 2. Build Tools (`node-gyp`)

*   `node-gyp` requires Python (v3.x recommended) and a C++ compiler.
*   **Windows:**
    *   Install the current version of Python from [python.org](https://www.python.org/).
    *   Install Visual Studio Build Tools (or a full Visual Studio version with C++ workload). You can install the necessary components by running this in an **administrator** PowerShell or CMD:
        ```bash
        npm install --global --production windows-build-tools
        ```
        (This installs Python and VS Build Tools components if needed).
*   **Linux:**
    *   Install Python, `make`, and a C++ compiler suite (like `g++`):
        ```bash
        # Debian/Ubuntu
        sudo apt update
        sudo apt install -y python3 make g++ build-essential

        # Fedora/CentOS/RHEL
        sudo dnf update
        sudo dnf install -y python3 make gcc-c++
        # or using yum: sudo yum install -y python3 make gcc-c++
        ```
*   **macOS:**
    *   Install Python from [python.org](https://www.python.org/) or via Homebrew (`brew install python`).
    *   Install Xcode Command Line Tools:
        ```bash
        xcode-select --install
        ```

### 3. PC/SC Library and Daemon

*   **Windows:** The necessary `Winscard` library is part of the operating system. No extra steps are usually needed.
*   **Linux:**
    *   Install the PCSC-lite library and development headers:
        ```bash
        # Debian/Ubuntu
        sudo apt install -y libpcsclite1 libpcsclite-dev pcscd

        # Fedora/CentOS/RHEL
        sudo dnf install -y pcsc-lite pcsc-lite-devel pcsc-tools
        # or using yum
        ```
    *   **Ensure the `pcscd` daemon is running:**
        ```bash
        sudo systemctl start pcscd
        sudo systemctl enable pcscd # Optional: start on boot
        # Verify status: systemctl status pcscd
        ```
*   **macOS:**
    *   Install PCSC-lite, usually via Homebrew:
        ```bash
        brew install pcsc-lite
        ```
    *   **Ensure the `pcscd` daemon is running.** It should typically start automatically after installation or on demand. You can check using `launchctl list | grep pcscd` or `ps aux | grep pcscd`. If not running, you might need to start it (though this is less common on macOS): `sudo launchctl load -w /System/Library/LaunchDaemons/com.apple.ifdreader.plist` (path might vary slightly).

## Installation

### Examples
*   **API:** 
    const pcsc = require('nfc-reader-nodejs');
```bash
npm install nfc-reader-nodejs
