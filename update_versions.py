import os
import requests
import zipfile
import shutil
from typing import Optional
import urllib3

# Disable SSL verification warnings
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

def download_and_extract(url: str, package_name: str) -> bool:
    try:
        print(f"Downloading {url}")
        response = requests.get(url, stream=True, verify=False)  # Added verify=False here
        if response.status_code == 200:
            zip_path = f"{package_name}.zip"
            
            # Download the zip file
            with open(zip_path, 'wb') as f:
                for chunk in response.iter_content(chunk_size=8192):
                    f.write(chunk)
            
            # Remove existing directory if it exists
            if os.path.exists(package_name):
                shutil.rmtree(package_name)
            
            # Create the package directory
            os.makedirs(package_name, exist_ok=True)
            
            # Extract the zip file
            with zipfile.ZipFile(zip_path, 'r') as zip_ref:
                # Get all files in the zip
                files = zip_ref.namelist()
                common_prefix = os.path.commonprefix(files)
                
                for file in files:
                    if file.startswith(common_prefix):
                        # Remove the common prefix and extract
                        extracted_path = file[len(common_prefix):]
                        if extracted_path:  # Skip if it's just the directory itself
                            try:
                                source = zip_ref.open(file)
                                target_path = os.path.join(package_name, extracted_path)
                                
                                # Create directories if needed
                                os.makedirs(os.path.dirname(target_path), exist_ok=True)
                                
                                # Only extract if it's a file, not a directory
                                if not extracted_path.endswith('/'):
                                    with open(target_path, 'wb') as target:
                                        shutil.copyfileobj(source, target)
                            except Exception as e:
                                print(f"Warning: Error extracting {file}: {str(e)}")
                                continue
            
            # Clean up the zip file
            os.remove(zip_path)
            return True
        else:
            print(f"Failed to download {url}: Status code {response.status_code}")
    except Exception as e:
        print(f"Error downloading/extracting {url}: {str(e)}")
    return False

def try_version_update(package: str, current_version: str, base_url: str) -> Optional[str]:
    # For advel.cz domains, just try to download the current version
    if "advel.cz" in base_url:
        url = f"{base_url}/{package}-{current_version}.zip"
        if download_and_extract(url, package):
            return current_version
        return None

    # Try current version first
    url = f"{base_url}/{package}-{current_version}.zip"
    if download_and_extract(url, package):
        # Try minor version increment
        current_major, current_minor = map(float, current_version.split('.'))
        minor_version = f"{int(current_major)}.{current_minor + 0.1}"
        minor_url = f"{base_url}/{package}-{minor_version}.zip"
        
        if download_and_extract(minor_url, package):
            return minor_version

        # Try major version increment
        major_version = f"{int(current_major + 1)}.0"
        major_url = f"{base_url}/{package}-{major_version}.zip"
        
        if download_and_extract(major_url, package):
            return major_version

        return current_version
    
    return None

def main():
    versions = {}
    updated = False

    # Read current versions
    with open('versions.txt', 'r') as f:
        for line in f:
            package, version = line.strip().split('=')
            versions[package] = version

    # Process each package
    for package, version in versions.items():
        print(f"Processing {package} version {version}")
        if package.startswith('mathlib') or package in ['bindiff', 'png', 'zcomp']:
            base_url = "http://public-domain.advel.cz/download"
        elif package == 'cellsplit':
            base_url = "https://www.cellsplit.org/download"
        else:
            base_url = "https://www.fixbrowser.org/download"

        new_version = try_version_update(package, version, base_url)
        if new_version:
            if new_version != version:
                versions[package] = new_version
                print(f"Updated {package} from {version} to {new_version}")
                updated = True
            else:
                print(f"Downloaded {package} version {version}")
        else:
            print(f"Failed to process {package}")

    # Update versions.txt if changes were made
    if updated:
        with open('versions.txt', 'w') as f:
            for package, version in sorted(versions.items()):
                f.write(f"{package}={version}\n")

if __name__ == "__main__":
    main()