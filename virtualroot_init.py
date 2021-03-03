
import urllib.request as request
import gzip
import re
from collections import defaultdict, OrderedDict
import os
from hashlib import sha256
import io
import tarfile

repo = "http://snapshot.debian.org/archive"
snapshot = "20210201T024125Z"
dist = "bullseye"
packages_gz_hash = {
    "i386" : "47173ff202bbb341af3f1ce5533e89d1adb015618540a96b73123ac811062d9b",
    "amd64": "1244f5285377c03c2e6ee3ea06ed9584ce964c08bbb769f1a8875fe5288aa7bb"
}
cache_folder = "./virtualroot/cache"
root_cache_folder = "./virtualroot/pkg"

# Because we aren't trying to create a full rootfs, many of the required dependicies are nonsense
excluded_dependencies_list = [
    "adduser",
    "init-system-helpers",
    "base-files",
    "debconf",
    "debconf-2.0",
    "lsb-base",
    "x11-common",
    "libsensors-config",
    "libopenal-data"
]

def excluded_package(name):
    name = name.split(':')[0]
    return name in excluded_dependencies_list

if not os.path.exists(cache_folder):
    os.makedirs(cache_folder)

class no_split:
    def split(self, *args):
        return []

def cached_http_read(url, uid, sha256_hash = None):
    cache_path = f"{cache_folder}/{uid}"
    if os.path.exists(cache_path):
        with open(cache_path, 'rb') as f:
            data = f.read()
            hash = sha256(data).hexdigest()
            if sha256_hash and hash == sha256_hash:
                return data
        print(f'Cached file {cache_path} failed checksum. Expected {sha256_hash}, got {hash}. Redownloading')
        os.path.delete(cache_path)

    print(f"downloading {url} ")
    with request.urlopen(url) as h:
        data = h.read()
        hash = sha256(data).hexdigest()
        if sha256_hash and hash != sha256_hash:
            raise Exception(f"downloaded file {url} failed checksum. Expected {sha256_hash}, got {hash}.")
        with open(cache_path, 'wb') as f:
            f.write(data)
        return data


def get_packages(arch):
    url = f"{repo}/debian/{snapshot}/dists/{dist}/main/binary-{arch}/Packages.gz"
    hash = packages_gz_hash[arch]
    package_gz = cached_http_read(url, f"Packages-{arch}-{snapshot}.gz", hash)
    package_list = gzip.decompress(package_gz).decode('utf-8')

    packages = {}
    package = None
    current_key = None

    feildre = re.compile(r'([A-Za-z0-9\-]*):\s*(.*)\s*') # Splits "Key: value\n"

    for line in package_list.splitlines():
        if line == "": # There is an empty newline between each package
            if package == None:
                raise Exception(f"Error parsing {url}")
            name = package["Name"]
            if name in packages:
                raise Exception(f"Duplicate package {name}")

            packages[name] = package
            package = None
            continue

        m = feildre.fullmatch(line)
        if m is None:
            if line.startswith(" ") and package:
                # lines with a leading space are continuations of the previous line's value
                package[current_key] += line
                continue
            raise Exception(f"Error parsing line '{line}' from {url}")

        (current_key, value) = m.groups()

        # First entry in a package should be Package: Name
        if current_key == "Package":
            package = {"Name": value}
        else:
            #if current_key == "Provides":
            #    print(package["Name"] + " provides " + value)
            package[current_key] = value

    return packages

depend_re = re.compile(r'^\s*([\S]+)(:?\s*\((.*)\))?\s*$')

def find_vitual_packages(packages):
    vritual_links = defaultdict(list)
    for name, package in packages.items():

        provides = package.get('Provides', no_split)
        if not provides:
            continue

        # a package can provide multiple things
        for provides in package.get('Provides', no_split).split(','):
            # provides might be versioned
            m = depend_re.match(provides)
            if not m:
                raise Exception(f"Error parsing Provides '{provides}' in {name}")

            vritual_links[m.group(1)].append(name)

    return dict(vritual_links)


package_list = {}
virtual_package_list = None
primary_arch = ""

def merge_multiarch(arch_package_lists):
    global primary_arch
    global virtual_package_list
    primary_arch = arch_package_lists[0][0]
    multiarch_vitual_packages = defaultdict(list)

    def is_co_installable(arch, package):
        if arch == primary_arch:
            return True # All packages of the primary arch are always installablevalid

        # TODO: do we need this?
        # if another arch hasn't already provided this package, technically it won't conflict
        #if package['Name'] not in package_list:
        #    return True

        # Otherwise, packages which set Multi-Arch: same are co-installable
        return package.get('Multi-Arch') == "same"

    for (arch, packages) in arch_package_lists:
        for name, package in packages.items():
            name = f"{name}:{arch}"
            if is_co_installable(arch, package):
                package_list[name] = package

        for virtual, providers in find_vitual_packages(packages).items():
            for package in [packages.get(x) for x in providers]:
                name = f"{package['Name']}:{arch}"
                virtual_name = f"{virtual}:{arch}"

                if arch != primary_arch and package.get('Multi-Arch') != "same":
                    continue # we only want virtual packages that point to co-installable packages

                multiarch_vitual_packages[virtual_name] += [name]

    virtual_package_list = dict(multiarch_vitual_packages)


def build_package_list():
    i386_packages = get_packages("i386")
    amd64_packages = get_packages("amd64")

    merge_multiarch([('amd64', amd64_packages), ('i386', i386_packages)])

build_package_list()

def get_depend_rules(package_info):
    return package_info.get('Pre-Depends', no_split).split(',') + package_info.get('Depends', no_split).split(',')

def get_conflicts(package_info):
    return package_info.get('Conflicts', no_split).split(',')

def get_pkg_name(depend_rule):
    m = depend_re.match(depend_rule)
    if not m:
        raise Exception(f"Error parsing depend '{depend_rule}'")
    return m.group(1)

def resolve_depends(depends, arch, required_by):
    arch = arch if arch != "all" else primary_arch

    def find_depend(depend_rule):
        # might return multiple options for a dependancy
        depend = get_pkg_name(depend_rule)
        depend_arch = f"{depend}:{arch}"
        if ':' in depend:
            depend_arch = depend
            depend, _ = depend.split(':')

        # Ignore this dependency
        if excluded_package(depend):
            return []

        if depend_arch in package_list:
            return [depend_arch]

        if depend_arch in virtual_package_list:
            return virtual_package_list[depend_arch]

        # check if a package for the primary arch can satisfy this dependency
        def check_primary_arch(pkg_name):
            depenency_info = package_list.get(pkg_name)
            if depenency_info and depenency_info.get('Multi-Arch') == "foreign" and not excluded_package(pkg_name):
                return True
            return False
        if check_primary_arch(f"{depend}:{primary_arch}"):
            return [f"{depend}:{primary_arch}"]

        # finally, check the virtual packages for the priamry arch
        for provider_name in virtual_package_list.get(f"{depend}:{primary_arch}", []):
            depends = []
            if check_primary_arch(f"{provider_name}:{primary_arch}"):
                depends.append(f"{provider_name}:{primary_arch}")
            if depends:
                return depends

        return Exception(f"Could not find dependency {depend_rule} required by {required_by}")

    resolved = []
    for depend_rule in depends:
        if '|' in depend_rule:
            sub_depends = []
            for sub_rule in depend_rule.split('|'):
                sub_depend = find_depend(sub_rule)
                if isinstance(sub_depend, list): # ignore exceptions/Nones
                    for depend in sub_depend:
                        if depend not in sub_depends: # deduplicate
                            sub_depends += [depend]
            if not depends:
                raise Exception
            resolved += [sub_depends]
        else:
            depend = find_depend(depend_rule)
            if isinstance(depend, Exception):
                raise depend
            if depend:
                resolved += [depend]

    return resolved

def highest_priority(pkg_list):
    priorities = ["", "extra", "optional", "standard",  "important", "required"]
    def get_prioity(pkg):
        pkg_info = package_list[pkg]
        return priorities.index(pkg_info.get("Priority", ""))
    ordered = sorted(pkg_list, key=get_prioity, reverse=True)

    if (get_prioity(ordered[0]) > get_prioity(ordered[1])):
        return ordered[0]

    return None


def build_depenency_tree(requested):
    selected = OrderedDict()
    to_install = resolve_depends(requested, primary_arch, "base")

    def add_with_depends(pkg):
        nonlocal to_install

        if pkg in selected:
            return

        selected[pkg] = True
        info = package_list.get(pkg)
        print(f"Added {pkg} {info['Version']}")
        if info:
            resolved = resolve_depends(get_depend_rules(info), info['Architecture'], pkg)
            to_install += [x for x in resolved if x] # filter out empty lists
        else:
            raise Exception(f"Couldn't find info for {pkg}")

    while to_install:
        #print(to_install)

        # First, resolve any naked dependencies on the list (only one choice)
        single_pkg, choice = next(iter([(x[0], x) for x in to_install if len(x) == 1]), (None, None))
        if single_pkg:
            to_install.remove(choice)
            add_with_depends(single_pkg)
            continue

        # Second, filter any choices where one choice is already selected
        for easy_choice in [choice for choice in to_install if any([True for pkg in choice if pkg in selected])]:
            print(f"{easy_choice} already installed")
            to_install.remove(easy_choice)

        # Third, find a choices where one package clearly has a higher priority
        # TODO: is this sane?
        priority_pkg, choice = next(iter([(highest_priority(x), x) for x in to_install if highest_priority(x)]), (None, None))
        if priority_pkg:
            print(F"chose {priority_pkg} from {choice} due to priority")
            to_install.remove(choice)
            add_with_depends(priority_pkg)
            continue

        # Finally, default to the first choice in the list
        # According to debian docs, this is default behaviour of auto-builders
        if to_install:
            choice = to_install.pop(0)
            print(F"chose {choice[0]} from {choice} because it's listed first")
            add_with_depends(choice[0])

    return list(selected.keys())

def check_for_conflicts(selected):
    print("Checking for conflicts... ", end="")
    for pkg in selected:
        pkg_info = package_list[pkg]
        for conflict in [get_pkg_name(rule) for rule in get_conflicts(pkg_info)]:
            if conflict in selected:
                raise Exception(f"{pkg} conflicts with {conflict}")
    print("None")

def ar_get(ar_data, filename):
    # I was just going to subprocess out to ar, but it doesn't accept files on stdin
    # so I've done a minimal reimplemented here
    if ar_data[0:8].decode('ascii') != "!<arch>\n":
        raise Exception("invalid ar archive")

    # support wildcards, because we want to match on both *.tar.xz and *.tar.gz
    if filename[-1:] == '*':
        padded_filename = filename[0:-1]
    else:
        padded_filename = filename + ' ' * (16 - len(filename))

    file_header_length = 60
    data = ar_data[8:] # remove header
    while data[0:len(padded_filename)].decode('ascii') != padded_filename and len(data) > file_header_length:
        filesize = int(data[48:58].decode('ascii'))
        seek = file_header_length + filesize
        seek += seek & 1 # align to even bytes
        data = data[seek:]


    # we have found our file
    if len(data) > file_header_length:
        filesize = int(data[48:58].decode('ascii'))
        if len(data) < (file_header_length + filesize):
            raise Exception(f"invalid ar archive (truncated)")
        return data[file_header_length:file_header_length+filesize]
    raise Exception(f"file {filename} not found in ar archive")

def filter(filename):
    exclude = [
        "./usr/share/",
        "./usr/lib/mime/",
        "./lib/systemd/system/",
        "./usr/lib/dbus-1.0/",
        "./usr/lib/sysusers.d/",
        "./usr/lib/tmpfiles.d/",
        "./etc/netconfig",
        "./etc/openal",
        "./etc/pulse",
        "./etc/bash",
        "./etc/skel",
        "./etc/xattr.conf"
    ]
    allow = [
        "./bin/",
        "./sbin/",
        "./usr/bin/",
        "./usr/sbin/",
        "./lib/i386-linux-gnu/",
        "./lib/x86_64-linux-gnu/",
        "./usr/lib/i386-linux-gnu/",
        "./usr/lib/x86_64-linux-gnu/",
        "./usr/libexec/",
        "./etc/ld.so.conf.d/",
        "./lib64",
    ]
    print(filename)
    for prefix in allow:
        if filename.startswith(prefix):
            return True
    for prefix in exclude:
        if filename.startswith(prefix):
            return False
    raise Exception("Unknown path", filename)

def download_packages(selected):
    overlay = []
    for pkg in selected:
        info = package_list[pkg]
        name = info['Name']
        url = f"{repo}/debian/{snapshot}/{info['Filename']}"
        hash = info['SHA256']
        shorthash = hash[0:8]
        version = info['Version']
        arch = info['Architecture']

        # pretty sure this should be unique
        uid = f"{name}_{version}_{arch}.deb"

        data = cached_http_read(url, uid, hash)

        print(name)

        data_tar = ar_get(data, "data.tar.*")
        control_tar = ar_get(data, "control.tar.*")
        files = []
        links = []
        symlinks = []

        # with tarfile.open(fileobj=io.BytesIO(control_tar), mode='r') as tar:
        #     md5sum = tar.extractfile("./md5sums")
        #     for line in md5sum.readlines():
        #         filename = line[34:-1].decode('ascii')
        #         if filter(filename):
        #             files += [filename]

        obj_root = f"{root_cache_folder}/{arch}_{name}_{shorthash}"

        with tarfile.open(fileobj=io.BytesIO(data_tar), mode='r') as tar:
            for m in tar.getmembers():
                if m.type == tarfile.DIRTYPE or not filter(m.path):
                    continue
                elif m.type == tarfile.REGTYPE:
                    files += [m]
                elif m.type == tarfile.LNKTYPE:
                    links += [m]
                elif m.type == tarfile.SYMTYPE:
                    symlinks += [m]
                else:
                    raise Exception("Unhandled type", m, m.type)
            for m in files:
                print(f"\t{m.path}")
                path = m.path[1:]
                cache_obj_path = f"{obj_root}/{m.path[1:]}"
                cache_obj_dir = os.path.dirname(cache_obj_path)
                if not os.path.exists(cache_obj_dir):
                    os.makedirs(cache_obj_dir)
                with open(cache_obj_path, 'wb') as dest:
                    dest.write(tar.extractfile(m).read())
                overlay.append(('file', path, os.path.abspath(cache_obj_path)))

                realroot = f"./realroot/{m.path[1:]}"
                realroot_dir = os.path.dirname(realroot)
                if not os.path.exists(cache_obj_dir):
                    os.makedirs(cache_obj_dir)
                with open(cache_obj_path, 'wb') as dest:
                    dest.write(tar.extractfile(m).read())
            for m in links:
                path = m.path[1:]
                link = f"{obj_root}/{m.linkpath[2:]}"
                overlay.append(('hard', path, os.path.abspath(link)))

                full_link = f"./realroot/{m.linkpath[2:]}"
                if not os.path.exists(cache_obj_dir):
                    os.makedirs(cache_obj_dir)
                os.link(full_link, path)
            for m in symlinks:
                path = m.path[1:]
                dest = f"./realroot/{m.linkpath[2:]}"
                if not os.path.exists(cache_obj_dir):
                    os.makedirs(cache_obj_dir)
                os.symlink(path, dest)
                overlay.append(('link', path, m.linkpath))

    print("\n\nOverlay:")
    for t, path, dst in overlay:
        print(t, path, dst)


selected = build_depenency_tree(["libpulse0:i386"])
selected = build_depenency_tree(["bash", "coreutils", "dbus", "usbutils", "pciutils"])
selected = build_depenency_tree(["libwayland-client0", "libx11-6", "libgl1-mesa-dri", "libopenal1", "libsdl1.2debian", "libsdl2-2.0-0"])
#selected = build_depenency_tree(["bash", "coreutils", "curl"])

check_for_conflicts(selected)
download_packages(selected)