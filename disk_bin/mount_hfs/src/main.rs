use std::env;

use std::fs;
use std::os::unix::fs::MetadataExt;

use std::ffi::CString;
use libc::{iovec, uid_t, c_int, c_char, gid_t, nmount, getpwnam, getgrnam, group, passwd};


struct MountOption {
    // These keep the data alive
    name: CString,
    value: Option<Vec<u8>>,
    // These are the iovec parts
    name_io: iovec,
    value_io: iovec,
}


impl MountOption {
    fn new(name: &str, value: Option<&[u8]>) -> Self {
        let name_cstr = CString::new(name).expect("Invalid name");
        let value_data = value.map(|v| v.to_vec());
        
        let name_io = iovec {
            iov_base: name_cstr.as_ptr() as *mut libc::c_void,
            iov_len: name_cstr.as_bytes().len() + 1,
        };
        
        let value_io = match value {
            Some(v) => iovec {
                iov_base: v.as_ptr() as *mut libc::c_void,
                iov_len: v.len(),
            },
            None => iovec {
                iov_base: std::ptr::null_mut(),
                iov_len: 0,
            },
        };
        
        MountOption {
            name: name_cstr,
            value: value_data,
            name_io,
            value_io,
        }
    }
}


fn build_mount_options(options: Vec<MountOption>) -> (Vec<iovec>, Vec<MountOption>) {
    
    let mut iovecs = Vec::new();
    for opt in &options {
        iovecs.push(opt.name_io);
        iovecs.push(opt.value_io);
    }
    
    (iovecs, options)
}


fn parse_flags(opt: &str) -> c_int {
    let mut flags = 0;

    for flag in opt.split(',') {
        match flag {
            "ro" => flags |= libc::MNT_RDONLY, 
            &_ => flags |= 0,
        }
    }

    flags
}


fn get_user_info(path: &str, user: Option<String>, grp: Option<String>) -> (uid_t, gid_t) {

    let uid: uid_t;
    let gid: gid_t;

    let attr = fs::metadata(path).unwrap();
    
    match user {
        Some(u) => {
            let user_c = CString::new(u).unwrap();
            let pw = unsafe { getpwnam(user_c.as_ptr()) };

            if !pw.is_null() {
                let pw: &passwd = unsafe { &*pw };
                uid = pw.pw_uid;
            } else {
                uid = attr.uid();
            }
        }, 
        None => {
            uid = attr.uid(); 
        }
    };

    match grp {
        Some(g) => {
            let group_c = CString::new(g).unwrap();
            let gr = unsafe { getgrnam(group_c.as_ptr()) };

            if !gr.is_null() {
                let gr: &group = unsafe { &*gr };
                gid = gr.gr_gid;
            } else {
                gid = attr.gid();
            }
        },
        None => {
            gid = attr.gid();
        }
    };

    (uid, gid)
}


fn usage() {
    let usg = r#"
Usage: mount_hfs [-o options] [-u user] [-g group] device mount_point

Arguments:
  device         Path to the device (e.g., /dev/md10)
  mount_point    Directory to mount the volume (e.g., /mnt)

Options:
  -o options     Comma-separated mount options, such as:
                   ro          Mount read-only
  -u user        Mount with user. Defaults to uid of mount_point
  -g group       Mount with group. Defaults to gid of mount_point
"#;
    println!("{}", usg);
    std::process::exit(1);
}


fn main() {
    let args: Vec<String> = env::args().collect();

    if args.len() < 3 {
        usage();
    }

    let mut device: Option<&str> = None;
    let mut mnt_path: Option<&str> = None;

    let mut options: Option<String> = None;
    let mut user_in: Option<String> = None;
    let mut grp_in: Option<String> = None;

    let mut idx = 1;
    
    while idx < args.len() {
        if args[idx] == "-o" {
            if args.len() <= idx + 2 {
                usage();
            }

            options = Some(args[idx + 1].clone());

            idx += 2;
            continue;
        }

        if args[idx] == "-u" {
            if args.len() <= idx + 2 {
                usage();
            }
            
            user_in = Some(args[idx + 1].clone());

            idx += 2;
            continue;
        }

        if args[idx] == "-g" {
            if args.len() <= idx + 2 {
                usage();
            }
            
            grp_in = Some(args[idx + 1].clone());

            idx += 2;
            continue;
        }

        if device.is_none() {
            device = Some(&args[idx]);
            idx += 1;
            continue;
        }

        if mnt_path.is_none() {
            mnt_path = Some(&args[idx]);
            idx += 1;
            continue;
        }
        
        usage();
    }

    if device.is_none() || mnt_path.is_none() {
        usage();
    }

    let (uid, gid) = get_user_info(mnt_path.unwrap(), user_in, grp_in);

    let flags: c_int = match &options {
        Some(opt) => parse_flags(opt), 
        None => 0,
    };

    let fstype = b"hfs\0";
    let fspath = [mnt_path.unwrap().as_bytes(), b"\0"].concat();
    let from = [device.unwrap().as_bytes(), b"\0"].concat();
    let hfs_uid = [uid.to_string().as_bytes(), b"\0"].concat();
    let hfs_gid = [gid.to_string().as_bytes(), b"\0"].concat();
    
    let options = vec![
        MountOption::new("fstype", Some(fstype)),
        MountOption::new("fspath", Some(&fspath)),
        MountOption::new("from", Some(&from)),
        MountOption::new("hfs_uid", Some(&hfs_uid)),
        MountOption::new("hfs_gid", Some(&hfs_gid)),
        MountOption::new("ro", None),
    ];

    let (iovecs, _options) = build_mount_options(options);

    let res = unsafe {
        nmount(
            iovecs.as_ptr() as *mut iovec, 
            iovecs.len() as u32,
            flags, 
        )
    };

    if res != 0 {
        println!("mount failed: {}", std::io::Error::last_os_error());
        std::process::exit(1);
    } 
}
