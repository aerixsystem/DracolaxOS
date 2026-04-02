import os
import re
import shutil
from pathlib import Path

# Configuration
EXTENSIONS = {'.c', '.h'}
BACKUP_DIR = ".include_backup"

def fix_includes(root_dir):
    project_map = {}
    modified_files = 0

    # 1. Map all files in the project for quick lookup
    print("Indexing project files...")
    for root, _, files in os.walk(root_dir):
        for file in files:
            if Path(file).suffix in EXTENSIONS:
                # Store the most "shallow" path if duplicates exist
                if file not in project_map:
                    project_map[file] = os.path.join(root, file)

    # 2. Regex to find local includes: #include "file.h"
    include_pattern = re.compile(r'#include\s+"([^"]+)"')

    # 3. Process files
    for root, _, files in os.walk(root_dir):
        if BACKUP_DIR in root: continue # Skip our own backup folder

        for file in files:
            if Path(file).suffix not in EXTENSIONS: continue
            
            file_path = os.path.join(root, file)
            with open(file_path, 'r') as f:
                content = f.readlines()

            new_content = []
            changed = False

            for line in content:
                match = include_pattern.search(line)
                if match:
                    original_path = match.group(1)
                    full_include_path = os.path.join(root, original_path)

                    # If the path is broken, try to fix it
                    if not os.path.exists(full_include_path):
                        filename_only = os.path.basename(original_path)
                        
                        if filename_only in project_map:
                            target_abs_path = project_map[filename_only]
                            # Calculate new relative path
                            new_rel_path = os.path.relpath(target_abs_path, root)
                            line = line.replace(original_path, new_rel_path)
                            changed = True
                            print(f"Fixed: {file} -> {filename_only}")
                
                new_content.append(line)

            if changed:
                # 4. Efficient Backup: Only back up modified files
                backup_path = os.path.join(root_dir, BACKUP_DIR, os.path.relpath(file_path, root_dir))
                os.makedirs(os.path.dirname(backup_path), exist_ok=True)
                shutil.copy2(file_path, backup_path)

                with open(file_path, 'w') as f:
                    f.writelines(new_content)
                modified_files += 1

    print(f"\nTask complete. {modified_files} files updated.")
    print(f"Originals saved in: {os.path.join(root_dir, BACKUP_DIR)}")

if __name__ == "__main__":
    # Runs in the current directory
    fix_includes(os.getcwd())