#!/usr/bin/env python3
"""
Bundle all qpackages.com dependencies locally in the repository.
This ensures builds work even if qpackages.com goes down.
"""

import json
import os
import sys
import zipfile
import shutil
from pathlib import Path
import requests

def download_file(url, dest_path):
    """Download a file from URL to destination path."""
    print(f"  Downloading: {url}")
    response = requests.get(url, stream=True)
    response.raise_for_status()
    
    with open(dest_path, 'wb') as f:
        for chunk in response.iter_content(chunk_size=8192):
            f.write(chunk)
    
    return dest_path

def extract_zip(zip_path, extract_to):
    """Extract a zip file to destination directory."""
    print(f"  Extracting to: {extract_to}")
    with zipfile.ZipFile(zip_path, 'r') as zip_ref:
        zip_ref.extractall(extract_to)
    os.remove(zip_path)

def bundle_dependency(dep_id, version, additional_data):
    """Bundle a single dependency from qpackages.com."""
    print(f"\nBundling {dep_id}@{version}...")
    
    extern_includes = Path("extern/includes")
    extern_libs = Path("extern/libs")
    
    extern_includes.mkdir(parents=True, exist_ok=True)
    extern_libs.mkdir(parents=True, exist_ok=True)
    
    # Try to get metadata from qpackages.com
    api_url = f"https://qpackages.com/{dep_id}/{version}"
    
    try:
        response = requests.get(api_url)
        if response.status_code == 200:
            metadata = response.json()
            print(f"  Found metadata on qpackages.com")
            
            # Download headers if available
            if 'headersUrl' in metadata:
                headers_url = metadata['headersUrl']
                zip_path = Path("/tmp") / f"{dep_id}_headers.zip"
                download_file(headers_url, zip_path)
                
                # Extract to extern/includes/{dep_id}
                extract_dir = extern_includes / dep_id
                extract_dir.mkdir(parents=True, exist_ok=True)
                extract_zip(zip_path, str(extract_dir))
                print(f"  ✓ Headers bundled to {extract_dir}")
            
            # Download libraries if available
            if 'soLink' in metadata:
                so_url = metadata['soLink']
                lib_name = additional_data.get('overrideSoName', f"lib{dep_id.replace('-', '_')}.so")
                lib_path = extern_libs / lib_name
                
                download_file(so_url, str(lib_path))
                print(f"  ✓ Library bundled to {lib_path}")
                
            return True
        else:
            print(f"  Not found on qpackages.com (HTTP {response.status_code})")
    except Exception as e:
        print(f"  Error fetching from qpackages.com: {e}")
    
    # Try GitHub as fallback
    repo = additional_data.get('repo')
    ref = additional_data.get('ref')
    
    if repo and ref:
        print(f"  Trying GitHub: {repo}@{ref}")
        
        # Try to download headers from GitHub releases
        release_url = f"https://github.com/{repo}/releases/download/{ref}/{dep_id}-headers.zip"
        zip_path = Path("/tmp") / f"{dep_id}_headers_gh.zip"
        
        try:
            download_file(release_url, zip_path)
            
            extract_dir = extern_includes / dep_id
            extract_dir.mkdir(parents=True, exist_ok=True)
            extract_zip(zip_path, str(extract_dir))
            print(f"  ✓ Headers bundled from GitHub to {extract_dir}")
            return True
        except Exception as e:
            print(f"  GitHub headers not available: {e}")
        
        # Try alternative: download source zip and extract includes
        source_url = f"https://github.com/{repo}/archive/refs/tags/{ref}.zip"
        zip_path = Path("/tmp") / f"{dep_id}_source.zip"
        
        try:
            download_file(source_url, zip_path)
            
            extract_dir = Path("/tmp") / f"{dep_id}_source"
            extract_dir.mkdir(parents=True, exist_ok=True)
            
            with zipfile.ZipFile(zip_path, 'r') as zip_ref:
                zip_ref.extractall(str(extract_dir))
            
            # Find and copy include directories
            source_extracted = list(extract_dir.glob("*"))[0] if list(extract_dir.glob("*")) else None
            if source_extracted:
                # Look for 'include' or 'includes' folder
                for inc_folder in ['include', 'includes', 'Include']:
                    inc_path = source_extracted / inc_folder
                    if inc_path.exists():
                        dest_inc = extern_includes / dep_id
                        dest_inc.mkdir(parents=True, exist_ok=True)
                        shutil.copytree(str(inc_path), str(dest_inc), dirs_exist_ok=True)
                        print(f"  ✓ Headers bundled from GitHub source to {dest_inc}")
                        return True
                
                # If no include folder, copy everything
                dest_inc = extern_includes / dep_id
                dest_inc.mkdir(parents=True, exist_ok=True)
                shutil.copytree(str(source_extracted), str(dest_inc), dirs_exist_ok=True)
                print(f"  ✓ All sources bundled from GitHub to {dest_inc}")
                return True
        except Exception as e:
            print(f"  GitHub source not available: {e}")
    
    print(f"  ⚠ Could not bundle {dep_id}@{version}")
    return False

def main():
    """Main function to bundle all dependencies."""
    deps_file = Path("scripts/dependencies.json")
    
    if not deps_file.exists():
        print("ERROR: scripts/dependencies.json not found!")
        sys.exit(1)
    
    with open(deps_file, 'r') as f:
        dependencies = json.load(f)
    
    print(f"Found {len(dependencies)} dependencies to bundle\n")
    
    success_count = 0
    fail_count = 0
    
    for dep in dependencies:
        dep_info = dep.get('dependency', {})
        dep_id = dep_info.get('id')
        version_range = dep_info.get('versionRange', '')
        additional_data = dep_info.get('additionalData', {})
        
        # Extract exact version from versionRange (e.g., "=0.5.0" -> "0.5.0")
        version = version_range.lstrip('=')
        
        if not dep_id:
            print(f"Skipping dependency without ID")
            continue
        
        if bundle_dependency(dep_id, version, additional_data):
            success_count += 1
        else:
            fail_count += 1
    
    print(f"\n{'='*60}")
    print(f"Bundling complete: {success_count} succeeded, {fail_count} failed")
    print(f"{'='*60}")
    
    if fail_count > 0:
        print("\nWarning: Some dependencies could not be bundled.")
        print("You may need to manually add them or fix their repository references.")
    
    return 0 if fail_count == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
