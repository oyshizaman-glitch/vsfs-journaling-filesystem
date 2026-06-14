# VSFS Journaling File System

A simple journaling file system simulator written in C. This project demonstrates how modern file systems use journaling to ensure consistency and recover from crashes.

## Features

- Create files using inode-based metadata management
- Journal metadata updates before applying them to disk
- Commit-based transaction handling
- Crash recovery through journal replay
- Virtual disk image support (`vsfs.img`)
- Directory and inode management

## Concepts Demonstrated

- File Systems
- Journaling
- rash Recovery
- Inodes
- Directory Entries
- Bitmaps
- Transactions
- Disk Block Management
- Low-Level File I/O
  
## Project Structure

```text
journal.c      - Main journaling implementation
vsfs.img       - Virtual file system image
```

## Build

Compile using GCC:

```bash
gcc journal.c -o journal
```

## Usage

### Create a File

```bash
./journal create filename
```

Example:

```bash
./journal create notes.txt
```

Output:

```text
Created notes.txt (inum X) in journal
```

### Install Journal

Apply all committed journal transactions to the file system:

```bash
./journal install
```

Output:

```text
Journal installed.
```

## How It Works

When a file is created:

1. Metadata blocks are loaded.
2. A free inode is allocated.
3. Directory entries are updated.
4. Changes are written to the journal.
5. A commit record is added.
6. Transactions can later be installed to the file system.

This approach ensures that committed operations can be recovered even after unexpected crashes.

## Learning Objectives

This project was developed to understand:

- How journaling file systems work
- Transaction-based updates
- Metadata consistency
- Crash recovery mechanisms
- Operating system storage concepts

## Author

Maria Zaman Oyshi
