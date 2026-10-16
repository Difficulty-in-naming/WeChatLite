use anyhow::{Result, ensure};
use std::{
    fs,
    path::{Path, PathBuf},
};

#[cfg(windows)]
mod native {
    use super::*;
    use std::{
        ffi::OsString,
        io,
        os::windows::{
            ffi::{OsStrExt, OsStringExt},
            io::AsRawHandle,
        },
        ptr,
    };
    use windows_sys::Win32::{
        Foundation::{CloseHandle, ERROR_ALREADY_EXISTS, LocalFree},
        Security::{
            Authorization::{
                ConvertSidToStringSidW, ConvertStringSecurityDescriptorToSecurityDescriptorW,
                GetNamedSecurityInfoW, SE_FILE_OBJECT,
            },
            DACL_SECURITY_INFORMATION, EqualSid, GetSecurityDescriptorOwner, GetTokenInformation,
            OWNER_SECURITY_INFORMATION, PROTECTED_DACL_SECURITY_INFORMATION, SECURITY_ATTRIBUTES,
            SetFileSecurityW, TOKEN_QUERY, TOKEN_USER, TokenUser,
        },
        Storage::FileSystem::{
            BY_HANDLE_FILE_INFORMATION, CreateDirectoryW, GetFileInformationByHandle,
        },
        System::{
            Com::CoTaskMemFree,
            Threading::{GetCurrentProcess, OpenProcessToken},
        },
        UI::{
            Shell::{FOLDERID_LocalAppData, SHGetKnownFolderPath},
            WindowsAndMessaging::{MB_ICONERROR, MB_OK, MessageBoxW},
        },
    };

    fn wide(value: &std::ffi::OsStr) -> Vec<u16> {
        value.encode_wide().chain(Some(0)).collect()
    }
    fn wide_string(value: &str) -> Vec<u16> {
        value.encode_utf16().chain(Some(0)).collect()
    }

    struct LocalMemory(*mut core::ffi::c_void);
    impl Drop for LocalMemory {
        fn drop(&mut self) {
            unsafe {
                LocalFree(self.0);
            }
        }
    }

    fn current_sid() -> Result<String> {
        unsafe {
            let mut token = ptr::null_mut();
            ensure!(
                OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) != 0,
                "cannot read current user token"
            );
            let mut length = 0;
            GetTokenInformation(token, TokenUser, ptr::null_mut(), 0, &mut length);
            let mut words = vec![0usize; (length as usize).div_ceil(std::mem::size_of::<usize>())];
            let ok = GetTokenInformation(
                token,
                TokenUser,
                words.as_mut_ptr().cast(),
                length,
                &mut length,
            );
            CloseHandle(token);
            ensure!(ok != 0, "cannot read current user SID");
            let user = &*(words.as_ptr().cast::<TOKEN_USER>());
            let mut text = ptr::null_mut();
            ensure!(
                ConvertSidToStringSidW(user.User.Sid, &mut text) != 0,
                "cannot format current user SID"
            );
            let _text = LocalMemory(text.cast());
            let mut n = 0;
            while *text.add(n) != 0 {
                n += 1;
            }
            Ok(String::from_utf16(std::slice::from_raw_parts(text, n))?)
        }
    }

    pub fn private_directory(path: &Path) -> Result<()> {
        let sid = current_sid()?;
        let sddl = wide_string(&format!("O:{sid}D:P(A;OICI;FA;;;{sid})(A;OICI;FA;;;SY)"));
        let name = wide(path.as_os_str());
        unsafe {
            let mut descriptor = ptr::null_mut();
            ensure!(
                ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    sddl.as_ptr(),
                    1,
                    &mut descriptor,
                    ptr::null_mut()
                ) != 0,
                "cannot construct private cache permissions"
            );
            let _descriptor = LocalMemory(descriptor);
            if CreateDirectoryW(
                name.as_ptr(),
                &SECURITY_ATTRIBUTES {
                    nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
                    lpSecurityDescriptor: descriptor,
                    bInheritHandle: 0,
                },
            ) == 0
            {
                let error = io::Error::last_os_error();
                ensure!(
                    error.raw_os_error() == Some(ERROR_ALREADY_EXISTS as i32),
                    "cannot create cache directory: {error}"
                );
            }
            ensure!(
                crate::reject_link(path)?.is_dir(),
                "cache path is not a directory"
            );
            let mut existing = ptr::null_mut();
            let mut owner = ptr::null_mut();
            let result = GetNamedSecurityInfoW(
                name.as_ptr(),
                SE_FILE_OBJECT,
                OWNER_SECURITY_INFORMATION,
                &mut owner,
                ptr::null_mut(),
                ptr::null_mut(),
                ptr::null_mut(),
                &mut existing,
            );
            let _existing = LocalMemory(existing);
            ensure!(result == 0, "cannot inspect cache ownership");
            let mut expected_owner = ptr::null_mut();
            let mut defaulted = 0;
            ensure!(
                GetSecurityDescriptorOwner(descriptor, &mut expected_owner, &mut defaulted) != 0
                    && EqualSid(owner, expected_owner) != 0,
                "cache directory belongs to another owner"
            );
            ensure!(
                SetFileSecurityW(
                    name.as_ptr(),
                    DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                    descriptor
                ) != 0,
                "cannot protect cache directory"
            );
        }
        Ok(())
    }

    pub fn cache_root() -> Result<PathBuf> {
        unsafe {
            let mut path = ptr::null_mut();
            let result =
                SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, ptr::null_mut(), &mut path);
            ensure!(
                result >= 0 && !path.is_null(),
                "cannot find current user's LocalAppData"
            );
            let mut n = 0;
            while *path.add(n) != 0 {
                n += 1;
            }
            let known = PathBuf::from(OsString::from_wide(std::slice::from_raw_parts(path, n)));
            CoTaskMemFree(path.cast());
            crate::check_ancestors(&known)?;
            Ok(fs::canonicalize(known)?.join("WeChatLitePortable"))
        }
    }

    pub fn regular_file(file: &fs::File) -> Result<()> {
        unsafe {
            let mut info: BY_HANDLE_FILE_INFORMATION = std::mem::zeroed();
            ensure!(
                GetFileInformationByHandle(file.as_raw_handle(), &mut info) != 0,
                "cannot inspect cache/source file"
            );
            ensure!(
                info.dwFileAttributes & 0x400 == 0 && info.nNumberOfLinks == 1,
                "linked files are not allowed"
            );
        }
        ensure!(file.metadata()?.is_file(), "not a regular file");
        Ok(())
    }

    pub fn show_error(message: &str) {
        unsafe {
            MessageBoxW(
                ptr::null_mut(),
                wide_string(message).as_ptr(),
                wide_string("微信轻聊便携包").as_ptr(),
                MB_OK | MB_ICONERROR,
            );
        }
    }
}

#[cfg(windows)]
pub use native::*;

#[cfg(not(windows))]
pub fn private_directory(path: &Path) -> Result<()> {
    use std::os::unix::fs::{DirBuilderExt, MetadataExt};
    if !path.exists() {
        fs::DirBuilder::new().mode(0o700).create(path)?;
    }
    let metadata = crate::reject_link(path)?;
    ensure!(
        metadata.is_dir() && metadata.mode() & 0o077 == 0,
        "cache directory is not private"
    );
    Ok(())
}

#[cfg(not(windows))]
pub fn regular_file(file: &fs::File) -> Result<()> {
    use std::os::unix::fs::MetadataExt;
    let metadata = file.metadata()?;
    ensure!(
        metadata.is_file() && metadata.nlink() == 1,
        "not a regular unlinked file"
    );
    Ok(())
}

#[cfg(not(windows))]
pub fn cache_root() -> Result<PathBuf> {
    anyhow::bail!("Windows portable launcher is Windows-only")
}

#[cfg(not(windows))]
pub fn show_error(message: &str) {
    eprintln!("{message}");
}
