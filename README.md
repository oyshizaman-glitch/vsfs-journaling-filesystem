# VSFS Journaling File System

A simplified journaling file system implemented in C that demonstrates transaction logging and crash recovery techniques used in modern file systems such as ext3 and ext4.

## Overview

This project operates on a virtual disk image (`vsfs.img`) and uses a write-ahead journal to ensure filesystem consistency. Instead of directly modifying filesystem metadata, updates are first recorded in a journal and committed as a transaction. The transaction can later be installed to the actual filesystem, allowing recovery from unexpected crashes.

## Features

* Inode-based file management
* Directory entry creation
* Inode bitmap allocation
* Write-ahead logging (WAL)
* Transaction commit records
* Journal replay and recovery
* Virtual disk block management
* Crash-safe metadata updates

## Journal Workflow

```text
Create File
     │
     ▼
Write Metadata Updates
     │
     ▼
Append Journal Records
     │
     ▼
Write Commit Record
     │
     ▼
Install Transaction
     │
     ▼
Update Filesystem Blocks
```

Only committed transactions are applied to the filesystem.

## Commands

### Create a File

```bash
gcc journal.c -o journal

./journal create file1.txt
```

### Install Journal Transactions

```bash
./journal install
```

## File System Concepts Used

* Journaling
* Transactions
* Inodes
* Directory Entries
* Bitmaps
* Disk Blocks
* Crash Recovery
* Metadata Consistency
* Low-Level File I/O

## Technologies

1. C
2. POSIX System Calls

  * open()
  * read()
  * write()
  * lseek()
3. GCC

## Educational Purpose

This project was developed to understand how journaling file systems maintain consistency and recover from failures using transaction-based updates.

## Author

Maria Zaman Oyshi

