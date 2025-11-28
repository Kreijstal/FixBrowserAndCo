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
        response = requests.get(url, stream=True, verify=False)
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
        # Parse version using integer arithmetic
        parts = current_version.split('.')
        current_major = int(parts[0])
        current_minor = int(parts[1])
        
        latest_version = current_version
        
        # Keep incrementing minor version until we hit a 404
        test_minor = current_minor + 1
        while True:
            test_version = f"{current_major}.{test_minor}"
            test_url = f"{base_url}/{package}-{test_version}.zip"
            if download_and_extract(test_url, package):
                latest_version = test_version
                test_minor += 1
            else:
                break
        
        # Try major version increment (e.g., 0.9 -> 1.0)
        major_version = f"{current_major + 1}.0"
        major_url = f"{base_url}/{package}-{major_version}.zip"
        
        if download_and_extract(major_url, package):
            latest_version = major_version
            # Continue checking minor versions for new major
            test_minor = 1
            while True:
                test_version = f"{current_major + 1}.{test_minor}"
                test_url = f"{base_url}/{package}-{test_version}.zip"
                if download_and_extract(test_url, package):
                    latest_version = test_version
                    test_minor += 1
                else:
                    break

        return latest_version
    
    return None

def get_base_url(package: str) -> str:
    # Dictionary of packages that belong to fixscript.org
    fixscript_packages = {
        'fixbuild', 'fixgui', 'fiximage', 'fixio', 'fixnative', 'fixscript',
        'fixscript-classes', 'fixscript-macros', 'fixscript-optional',
        'fixscript-unpack', 'fixtask', 'fixutil'
    }

    if package.startswith('mathlib') or package in ['bindiff', 'png', 'zcomp']:
        return "http://public-domain.advel.cz/download"
    elif package == 'cellsplit':
        return "https://www.cellsplit.org/download"
    elif package in fixscript_packages:
        return "https://www.fixscript.org/download"
    else:
        return "https://www.fixbrowser.org/download"

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
        base_url = get_base_url(package)
        
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