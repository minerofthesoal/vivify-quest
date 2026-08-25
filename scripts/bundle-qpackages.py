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
    try:
        response = requests.get(url, stream=True, timeout=30)
        response.raise_for_status()
        
        with open(dest_path, 'wb') as f:
            for chunk in response.iter_content(chunk_size=8192):
                f.write(chunk)
        
        return True
    except Exception as e:
        print(f"  ERROR: Failed to download: {e}")
        return False

def extract_zip(zip_path, extract_to):
    """Extract a zip file to destination directory."""
    print(f"  Extracting to: {extract_to}")
    try:
        with zipfile.ZipFile(zip_path, 'r') as zip_ref:
            zip_ref.extractall(extract_to)
        os.remove(zip_path)
        return True
    except Exception as e:
        print(f"  ERROR: Failed to extract: {e}")
        return False

def bundle_dependency(dep_id, version, additional_data, force_rebundle=False):
    """Bundle a single dependency from qpackages.com or GitHub."""
    print(f"\n{'='*60}")
    print(f"Bundling {dep_id}@{version}...")
    print(f"{'='*60}")
    
    extern_includes = Path("extern/includes")
    extern_libs = Path("extern/libs")
    
    extern_includes.mkdir(parents=True, exist_ok=True)
    extern_libs.mkdir(parents=True, exist_ok=True)
    
    # Check if already bundled
    include_dir = extern_includes / dep_id
    lib_name = additional_data.get('overrideSoName', f"lib{dep_id.replace('-', '_')}.so")
    lib_path = extern_libs / lib_name
    
    if include_dir.exists() and not force_rebundle:
        print(f"  ✓ Already bundled (use --force to re-bundle)")
        # Still check for library
        if 'soLink' in additional_data and not lib_path.exists():
            print(f"  Library missing, will download...")
        else:
            return True
    
    # Try GitHub first if repo/ref are available
    repo = additional_data.get('repo')
    ref = additional_data.get('ref')
    
    if repo and ref:
        print(f"\n  Trying GitHub: {repo}@{ref}")
        
        # Try to download headers from GitHub releases
        release_urls = [
            f"https://github.com/{repo}/releases/download/{ref}/{dep_id}-headers.zip",
            f"https://github.com/{repo}/releases/download/{ref}/{dep_id.replace('-', '_')}-headers.zip",
            f"https://github.com/{repo}/releases/download/{ref}/headers.zip",
        ]
        
        for release_url in release_urls:
            zip_path = Path("/tmp") / f"{dep_id}_headers_gh.zip"
            
            try:
                if download_file(release_url, str(zip_path)):
                    if extract_zip(str(zip_path), str(include_dir)):
                        print(f"  ✓ Headers bundled from GitHub release to {include_dir}")
                        
                        # Also download library if soLink is available
                        so_link = additional_data.get('soLink')
                        if so_link:
                            lib_name = additional_data.get('overrideSoName', f"lib{dep_id.replace('-', '_')}.so")
                            lib_path = extern_libs / lib_name
                            if download_file(so_link, str(lib_path)):
                                print(f"  ✓ Library bundled to {lib_path}")
                        
                        return True
            except Exception as e:
                continue
        
        # Try alternative: download source zip and extract includes
        source_url = f"https://github.com/{repo}/archive/refs/tags/{ref}.zip"
        zip_path = Path("/tmp") / f"{dep_id}_source.zip"
        
        try:
            print(f"  Trying source archive: {source_url}")
            if download_file(source_url, str(zip_path)):
                extract_dir = Path("/tmp") / f"{dep_id}_source"
                extract_dir.mkdir(parents=True, exist_ok=True)
                
                with zipfile.ZipFile(zip_path, 'r') as zip_ref:
                    zip_ref.extractall(str(extract_dir))
                
                # Find and copy include directories
                source_extracted = list(extract_dir.glob("*"))[0] if list(extract_dir.glob("*")) else None
                if source_extracted:
                    # Look for 'include' or 'includes' folder
                    for inc_folder in ['include', 'includes', 'Include', 'extern/includes']:
                        inc_path = source_extracted / inc_folder
                        if inc_path.exists():
                            dest_inc = extern_includes / dep_id
                            dest_inc.mkdir(parents=True, exist_ok=True)
                            shutil.copytree(str(inc_path), str(dest_inc), dirs_exist_ok=True)
                            print(f"  ✓ Headers bundled from GitHub source to {dest_inc}")
                            
                            # Download library if available
                            so_link = additional_data.get('soLink')
                            if so_link:
                                lib_name = additional_data.get('overrideSoName', f"lib{dep_id.replace('-', '_')}.so")
                                lib_path = extern_libs / lib_name
                                if download_file(so_link, str(lib_path)):
                                    print(f"  ✓ Library bundled to {lib_path}")
                            
                            return True
                    
                    # If no include folder, copy everything
                    dest_inc = extern_includes / dep_id
                    dest_inc.mkdir(parents=True, exist_ok=True)
                    shutil.copytree(str(source_extracted), str(dest_inc), dirs_exist_ok=True)
                    print(f"  ✓ All sources bundled from GitHub to {dest_inc}")
                    
                    # Download library if available
                    so_link = additional_data.get('soLink')
                    if so_link:
                        lib_name = additional_data.get('overrideSoName', f"lib{dep_id.replace('-', '_')}.so")
                        lib_path = extern_libs / lib_name
                        if download_file(so_link, str(lib_path)):
                            print(f"  ✓ Library bundled to {lib_path}")
                    
                    return True
        except Exception as e:
            print(f"  GitHub source not available: {e}")
    
    # Try qpackages.com as fallback
    api_url = f"https://qpackages.com/{dep_id}/{version}"
    
    print(f"\n  Trying qpackages.com: {api_url}")
    try:
        response = requests.get(api_url, timeout=10)
        if response.status_code == 200:
            metadata = response.json()
            print(f"  ✓ Found metadata on qpackages.com")
            
            # Extract repo/ref from config if available
            info = metadata.get('config', {}).get('info', {})
            branch_name = info.get('branchName') or info.get('additionalData', {}).get('branchName')
            repo_from_config = info.get('url', '').replace('https://github.com/', '')
            
            success = True
            
            # Try to get headers from GitHub using branch name from config
            if branch_name and repo_from_config:
                source_url = f"https://github.com/{repo_from_config}/archive/refs/heads/{branch_name}.zip"
                zip_path = Path("/tmp") / f"{dep_id}_config.zip"
                
                try:
                    print(f"  Trying config-based source: {source_url}")
                    if download_file(source_url, str(zip_path)):
                        extract_dir = Path("/tmp") / f"{dep_id}_config"
                        extract_dir.mkdir(parents=True, exist_ok=True)
                        
                        with zipfile.ZipFile(zip_path, 'r') as zip_ref:
                            zip_ref.extractall(str(extract_dir))
                        
                        source_extracted = list(extract_dir.glob("*"))[0] if list(extract_dir.glob("*")) else None
                        if source_extracted:
                            for inc_folder in ['include', 'includes', 'Include', 'extern/includes']:
                                inc_path = source_extracted / inc_folder
                                if inc_path.exists():
                                    dest_inc = extern_includes / dep_id
                                    dest_inc.mkdir(parents=True, exist_ok=True)
                                    shutil.copytree(str(inc_path), str(dest_inc), dirs_exist_ok=True)
                                    print(f"  ✓ Headers bundled from config to {dest_inc}")
                                    success = True
                                    break
                except Exception as e:
                    print(f"  Config-based source failed: {e}")
            
            # Download libraries if available
            so_link = additional_data.get('soLink') or metadata.get('config', {}).get('info', {}).get('additionalData', {}).get('soLink')
            if so_link:
                lib_name = additional_data.get('overrideSoName', f"lib{dep_id.replace('-', '_')}.so")
                lib_path = extern_libs / lib_name
                
                if download_file(so_link, str(lib_path)):
                    print(f"  ✓ Library bundled to {lib_path}")
                else:
                    print(f"  ⚠ Library download failed, but continuing...")
            
            return success
        else:
            print(f"  Not found on qpackages.com (HTTP {response.status_code})")
    except Exception as e:
        print(f"  Error fetching from qpackages.com: {e}")
    
    # Try CDN fallback for known packages
    cdn_urls = {
        'libil2cpp': f"https://cdn.frozenalex.com/libil2cpp_{version}.zip",
    }
    
    if dep_id in cdn_urls:
        cdn_url = cdn_urls[dep_id]
        print(f"\n  Trying CDN fallback: {cdn_url}")
        zip_path = Path("/tmp") / f"{dep_id}_cdn.zip"
        
        try:
            if download_file(cdn_url, str(zip_path)):
                if extract_zip(str(zip_path), str(include_dir)):
                    print(f"  ✓ Headers bundled from CDN to {include_dir}")
                    return True
        except Exception as e:
            print(f"  CDN fallback failed: {e}")
    
    print(f"\n  ⚠ Could not bundle {dep_id}@{version}")
    return False

def main():
    """Main function to bundle all dependencies."""
    deps_file = Path("scripts/dependencies.json")
    
    if not deps_file.exists():
        print("ERROR: scripts/dependencies.json not found!")
        sys.exit(1)
    
    # Check for force rebundle flag
    force_rebundle = os.environ.get('FORCE_REBUNDLE', 'false').lower() == 'true'
    if force_rebundle:
        print("Force re-bundle mode enabled")
    
    with open(deps_file, 'r') as f:
        data = json.load(f)
    
    # Handle both old format (list of {dependency: {...}}) and new format (list of {...})
    dependencies = data if isinstance(data, list) else data.get('dependencies', [])
    
    print(f"Found {len(dependencies)} dependencies to bundle\n")
    
    success_count = 0
    fail_count = 0
    skip_count = 0
    
    for dep in dependencies:
        # Handle both formats
        if isinstance(dep, dict):
            if 'dependency' in dep:
                dep_info = dep['dependency']
            else:
                dep_info = dep
        else:
            print(f"Skipping invalid dependency entry: {dep}")
            skip_count += 1
            continue
        
        dep_id = dep_info.get('id')
        version_range = dep_info.get('versionRange', '') or dep_info.get('version', '')
        additional_data = dep_info.get('additionalData', {})
        
        # Extract exact version from versionRange (e.g., "=0.5.0" -> "0.5.0")
        version = version_range.lstrip('=').lstrip('^').lstrip('~')
        
        if not dep_id:
            print(f"Skipping dependency without ID")
            skip_count += 1
            continue
        
        if bundle_dependency(dep_id, version, additional_data, force_rebundle):
            success_count += 1
        else:
            fail_count += 1
    
    print(f"\n{'='*60}")
    print(f"Bundling Summary:")
    print(f"  Successful: {success_count}")
    print(f"  Failed: {fail_count}")
    print(f"  Skipped: {skip_count}")
    print(f"{'='*60}")
    
    # List what was bundled
    print(f"\nBundled Includes ({len(list(Path('extern/includes').glob('*')))} packages):")
    for inc in sorted(Path('extern/includes').glob('*')):
        print(f"  - {inc.name}")
    
    print(f"\nBundled Libraries ({len(list(Path('extern/libs').glob('*')))} files):")
    for lib in sorted(Path('extern/libs').glob('*')):
        print(f"  - {lib.name}")
    
    if fail_count > 0:
        print("\n⚠ Warning: Some dependencies could not be bundled.")
        print("The build may still work if GitHub fallbacks are available in dependencies.json")
    
    return 0 if fail_count == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
