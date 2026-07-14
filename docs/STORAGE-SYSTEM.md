# DracolaxOS — Storage SSystem Architecture

```
Name: Dracolax
Language: C
Kernel: Custom
Storage: draco://storage/...

/storage                         → Root of all storage nodes
  /main      → Internal disk (Node ID: 01)
    /system (ID2: 001)          → OS core files, drivers, services
    /users  (ID2: 002)           → User accounts
      /<username> (ID2: 00201)
        /documents (tags: docs, project)
        /downloads (tags: downloads)
        /desktop (tags: ui, workspace)
        /configs (tags: settings)
        /temp (tags: temp, user) → Temporary files, hidden by default
    /apps (ID2: 003)            → Installed applications
    /temp (ID2: 004)             → System-wide temporary files
    /cache (ID2: 005)            → System cache (updates, thumbnails, logs)
    /logs (ID2: 006)             → System logs
  /usb       (Node ID: 02)
    /<device_name> (ID2: 0201)
      /files
      /apps
      /temp
  /network   (Node ID: 03)
    /<network_name> (ID2: 0301)
      /files
      /temp
  /ramdisk   (Node ID: 04)       → Volatile, fast storage
    /sessions (tags: session)
    /temp (tags: temp, fast)
  /apps      (Node ID: 05)       → Global apps
    /<app_name>
      /data
      /configs
      /cache

Packages Install: draco install <package>

Shutdown / Reboot / Restart:
  - Auto-cleanup: system temp, user temp, RAM-disk cleared
  - Session restore: open apps, unsaved work preserved
  - Safe unmount: USB/network storage checked before shutdown

AI Search:
  - Semantic tags, file type, user, storage node, time_range
  - Natural language queries supported
  - Results ranked by relevance, usage, and storage speed
  - Suggestions for related files if query yields few results
  - Fast access via Node ID + Folder/File ID2 + tags

App Management & Shortcuts:
  - Apps inherit system-wide shortcuts by default
  - Dynamic shortcut updates: conflicts prompt user without losing progress
  - Per-app overrides possible
  - Shortcut changes propagate to apps in real-time

Users & Permissions:
  - /storage/main/users/<username>
  - Read/write/execute permissions per folder and app
  - User-specific temp and configs separated

Logging & Monitoring:
  - /storage/main/logs
  - Tracks system events, app events, AI decisions
  - Filterable by time, user, severity

Memory / Performance Enhancements:
  - Unique Node IDs (ID1) and Folder/File IDs (ID2)
  - Tag-based indexing for fast search
  - RAM cache for frequently accessed IDs and tags
  - Snapshots/versioning support for system and user files

Core filesystem object model, Example:
ObjectID
ParentID
NodeID
Permissions
Tags
Timestamps

Metadata layer from path layer:
Paths become a navigation interface.
IDs remain the real reference.

etc..
```