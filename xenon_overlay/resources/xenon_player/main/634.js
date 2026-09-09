"use strict";
exports.ids = ['634'];
exports.modules = {
2823: (function (__unused_webpack_module, __webpack_exports__, __webpack_require__) {
// ESM COMPAT FLAG
__webpack_require__.r(__webpack_exports__);

// EXPORTS
__webpack_require__.d(__webpack_exports__, {
  xmpSettingConfigStore: () => (/* binding */ xmpSettingConfigStore),
  getXmpCachePath: () => (/* binding */ getXmpCachePath)
});

;// CONCATENATED MODULE: ../common/fs-utilities.ts
/**
 * @description: nodejs 文件相关的 async/await 的封装, AW 后缀为 async/await 缩写
 * @author: chenguideng
 */ const fs = __webpack_require__(7147);
const path = __webpack_require__(1017);
const util = __webpack_require__(3837);
const readline = __webpack_require__(4521);
const promisify = util.promisify;
(function(FileSystemAWNS) {
    FileSystemAWNS.MAX_PATH = 260;
    async function readFileAW(filePath, options) {
        let ret = new Buffer('');
        if (filePath !== undefined) {
            const readFile = promisify(fs.readFile);
            try {
                ret = await readFile(filePath, options);
            } catch (err) {
                console.log(err);
            }
        }
        return ret;
    }
    /**
   * @description 读取文件<br />
   * @method readFileAW
   * @param {string} filePath 文件路径
   * @return {Buffer} 成功为 Buffer 数据对象，失败为 null
   * @example
   * let data: Buffer = await readFileAW('d:/1.log'); <br />
   * @static
   */ FileSystemAWNS.readFileAW = readFileAW;
    async function readLineAw(filePath) {
        let ret = [];
        do {
            if (!filePath) {
                break;
            }
            if (!await existsAW(filePath)) {
                break;
            }
            ret = await new Promise((resolve)=>{
                const lines = [];
                const input = fs.createReadStream(filePath);
                const instance = readline.createInterface({
                    input
                });
                instance.on('line', (line)=>{
                    lines.push(line);
                });
                instance.on('close', ()=>{
                    resolve(lines);
                });
            });
        }while (0);
        return ret;
    }
    /**
   * @description 按行读取文件<br />
   * @method readLineAw
   * @param {string} filePath 文件路径
   * @return {string[]} 成功为 string 数组，失败为 null
   * @example
   * let lines: string[] = await readLineAw('d:/1.log'); <br />
   * @static
   */ FileSystemAWNS.readLineAw = readLineAw;
    async function writeFileAW(filePath, data) {
        let ret = false;
        if (filePath !== undefined && data !== null) {
            const writeFile = promisify(fs.writeFile);
            try {
                await writeFile(filePath, data);
                ret = true;
            } catch (err) {
                console.error(err);
            }
        }
        return ret;
    }
    /**
   * @description 写文件<br />
   * @method writeFileAW
   * @param {string} filePath 文件路径
   * @param {any} data 文件数据
   * @return {boolean} 成功为 true，失败为 false
   * @example
   * let result: boolean = await writeFileAW('d:/1.log', 'abc'); <br />
   * @static
   */ FileSystemAWNS.writeFileAW = writeFileAW;
    async function existsAW(filePath) {
        let ret = false;
        if (filePath !== undefined) {
            // 使用promisify时碰到类似'D:\xunlei\.' 这种后面带了个点的路径, 会一直认为文件存在
            const exists = fs.promises.access;
            try {
                await exists(filePath);
                ret = true;
            } catch (err) {
            // logger.information(err);
            }
        }
        return ret;
    }
    /**
   * @description 判断文件是否存在<br />
   * @method existsAW
   * @param {string} filePath 文件路径
   * @return {boolean} 成功为 true，失败为 false
   * @example
   * let result: boolean = await existsAW('d:/1.log'); <br />
   * @static
   */ FileSystemAWNS.existsAW = existsAW;
    async function dirExistsAW(filePath) {
        let ret = false;
        do {
            ret = await existsAW(filePath);
            if (!ret) {
                break;
            }
            const stats = await lstatAW(filePath);
            if (!stats) {
                break;
            }
            ret = stats.isDirectory();
        }while (0);
        return ret;
    }
    FileSystemAWNS.dirExistsAW = dirExistsAW;
    async function mkdirAW(dirName) {
        dirName = getValidDirName(dirName);
        let ret = false;
        if (dirName !== undefined) {
            const mkdir = promisify(fs.mkdir);
            try {
                await mkdir(dirName);
                ret = true;
            } catch (err) {
                console.error(err);
            }
        }
        return ret;
    }
    /**
   * @description 创建文件夹, 只能创建父文件夹存在的文件夹<br />
   * @method mkdirAW
   * @param {string} dirName 文件夹路径
   * @return {boolean} 成功为 true，失败为 false
   * @example
   * let result: boolean = await mkdirAW('d:/1/2'); <br />
   * @static
   */ FileSystemAWNS.mkdirAW = mkdirAW;
    async function rmdirAW(dirName) {
        let ret = false;
        if (dirName !== undefined) {
            const rmdir = promisify(fs.rmdir);
            try {
                await rmdir(dirName);
                ret = true;
            } catch (err) {
                console.error(err);
            }
        }
        return ret;
    }
    /**
   * @description 删除文件夹, 只能删除空文件夹<br />
   * @method rmdirAW
   * @param {string} dirName 文件夹路径
   * @return {boolean} 成功为 true，失败为 false
   * @example
   * let result: boolean = await rmdirAW('d:/1/2'); <br />
   * @static
   */ FileSystemAWNS.rmdirAW = rmdirAW;
    async function unlinkAW(filePath) {
        let ret = false;
        if (filePath !== undefined) {
            const unlink = promisify(fs.unlink);
            try {
                await unlink(filePath);
                ret = true;
            } catch (err) {
                console.error(err);
            }
        }
        return ret;
    }
    /**
   * @description 删除文件<br />
   * @method unlinkAW
   * @param {string} filePath 文件路径
   * @return {boolean} 成功为 true，失败为 false
   * @example
   * let result: boolean = await unlinkAW('d:/1.log'); <br />
   * @static
   */ FileSystemAWNS.unlinkAW = unlinkAW;
    async function readdirAW(dirName) {
        let ret = [];
        if (dirName !== undefined) {
            const readdir = promisify(fs.readdir);
            try {
                ret = await readdir(dirName);
            } catch (err) {
                console.error(err);
            }
        }
        return ret;
    }
    /**
   * @description 获取文件夹下子文件及子文件夹名称<br />
   * @method readdirAW
   * @param {string} dirName 文件夹路径
   * @return {string[]} 成功为 string[] 对象，失败为 null
   * @example
   * let result: string[] = await readdirAW('d:/1/'); <br />
   * @static
   */ FileSystemAWNS.readdirAW = readdirAW;
    async function lstatAW(filePath) {
        let ret;
        if (filePath !== undefined) {
            const lstat = promisify(fs.lstat);
            try {
                ret = await lstat(filePath);
            } catch (err) {
                console.error(err);
            }
        }
        return ret;
    }
    /**
   * @description 获取文件属性<br />
   * @method lstatAW
   * @param {string} filePath 文件路径
   * @return {fs.Stats} 成功为 fs.Stats 对象，失败为 null
   * @example
   * let result: fs.Stats = await lstatAW('d:/1.log'); <br />
   * @static
   */ FileSystemAWNS.lstatAW = lstatAW;
    async function rmSubdirAW(dirName, fileName) {
        let ret = false;
        if (dirName !== undefined && fileName !== undefined) {
            const filePath = path.join(dirName, fileName);
            const stats = await lstatAW(filePath);
            if (stats && stats.isDirectory()) {
                ret = await rmdirsAW(filePath);
            } else {
                ret = await unlinkAW(filePath);
            }
        }
        return ret;
    }
    FileSystemAWNS.rmSubdirAW = rmSubdirAW;
    async function rmdirsAW(dirName) {
        let ret = false;
        if (dirName !== undefined) {
            const isDir = await existsAW(dirName);
            if (isDir) {
                ret = true;
                const files = await readdirAW(dirName) || [];
                for(let i = 0; i < files.length; i++){
                    ret = await rmSubdirAW(dirName, files[i]) && ret;
                }
                if (ret || files.length === 0) {
                    ret = await rmdirAW(dirName) && ret;
                }
            }
        }
        return ret;
    }
    /**
   * @description 删除文件夹, 连带文件夹里面的子文件一并删除<br />
   * @method rmdirsAW
   * @param {string} dirName 文件夹路径
   * @return {boolean} 成功为 true，失败为 false
   * @example
   * let result: boolean = await rmdirsAW('d:/2/'); <br />
   * @static
   */ FileSystemAWNS.rmdirsAW = rmdirsAW;
    async function mkdirsAW(dirName) {
        let ret = false;
        dirName = getValidDirName(dirName);
        if (dirName !== undefined && dirName !== '' && dirName !== '.' && dirName !== '..') {
            if (await existsAW(dirName)) {
                ret = true;
            } else {
                if (path.dirname(dirName) === dirName) {
                    ret = false;
                } else if (await mkdirsAW(path.dirname(dirName))) {
                    ret = await mkdirAW(dirName);
                }
            }
        }
        return ret;
    }
    /**
   * @description 创建文件夹, 当父文件夹不存在时也会创建父文件夹<br />
   * @method mkdirsAW
   * @param {string} dirName 文件夹路径
   * @return {boolean} 成功为 true，失败为 false
   * @example
   * let result: boolean = await mkdirsAW('d:/2/3/'); <br />
   * @static
   */ FileSystemAWNS.mkdirsAW = mkdirsAW;
    async function renameAW(oldPath, newPath, errCallback) {
        let ret = false;
        newPath = getValidDirName(newPath);
        if (oldPath !== undefined && newPath !== undefined && (oldPath === null || oldPath === void 0 ? void 0 : oldPath.toLowerCase()) !== (newPath === null || newPath === void 0 ? void 0 : newPath.toLowerCase())) {
            const rename = promisify(fs.rename);
            try {
                await rename(oldPath, newPath);
                ret = true;
            } catch (err) {
                errCallback === null || errCallback === void 0 ? void 0 : errCallback(err);
                console.error(err);
            }
        }
        return ret;
    }
    /**
   * @description 移动文件；跨分区重命名文件，会有权限问题
   * @example
   * let ret: boolean = await renameAW('old.txt', 'new.txt');
   */ FileSystemAWNS.renameAW = renameAW;
    async function copyFileAW(src, dest) {
        let result;
        if (src.toLowerCase() !== dest.toLowerCase() && await existsAW(src)) {
            const readStream = fs.createReadStream(src);
            const writeStream = fs.createWriteStream(dest);
            result = new Promise((resolve)=>{
                const pipeWriteStram = readStream.pipe(writeStream);
                pipeWriteStram.on('finish', ()=>{
                    resolve(true);
                });
                // 需要主动捕获error事件: 可能磁盘空间不足，导致拷贝失败，这里流程不让中断先 error.code === 'ENOSPC'
                pipeWriteStram.on('error', (err)=>{
                    resolve(false);
                });
            });
        } else {
            result = new Promise((resolve)=>{
                resolve(false);
            });
        }
        return result;
    }
    /**
   * @description: 复制文件
   * @param src 要被拷贝的源文件名称
   * @param dest 拷贝操作的目标文件名
   * @returns 成功返回true，否则false
   * @example
   * let ret: boolean = await copyFileAW('src.txt', 'dest.txt',  fs.constants.COPYFILE_EXCL);
   */ FileSystemAWNS.copyFileAW = copyFileAW;
    async function copyDirsAW(src, dest) {
        let ret = false;
        let stats = await lstatAW(src);
        if (stats) {
            if (stats.isDirectory()) {
                ret = await mkdirsAW(dest);
                const files = await readdirAW(src) || []; // 此处只获取第一层，不递归
                for(let i = 0; i < files.length; i++){
                    var _dest_toLowerCase;
                    const filePath = path.join(src, files[i]);
                    const newPath = path.join(dest, files[i]);
                    if ((dest === null || dest === void 0 ? void 0 : (_dest_toLowerCase = dest.toLowerCase()) === null || _dest_toLowerCase === void 0 ? void 0 : _dest_toLowerCase.indexOf(filePath === null || filePath === void 0 ? void 0 : filePath.toLowerCase())) === 0) {
                        ret = true;
                        continue;
                    }
                    ret = await existsAW(filePath);
                    if (ret) {
                        stats = await lstatAW(filePath);
                        if (stats) {
                            if (stats.isDirectory()) {
                                ret = await copyDirsAW(filePath, newPath);
                            } else {
                                ret = await copyFileAW(filePath, newPath);
                            }
                        }
                    }
                }
            }
        }
        return ret;
    }
    FileSystemAWNS.copyDirsAW = copyDirsAW;
    async function moveDirsAW(src, dest) {
        let ret = false;
        let stats = await lstatAW(src);
        if (stats) {
            if (stats.isDirectory()) {
                ret = await mkdirsAW(dest);
                const files = await readdirAW(src) || []; // 此处只获取第一层，不递归
                for(let i = 0; i < files.length; i++){
                    var _dest_toLowerCase;
                    const filePath = path.join(src, files[i]);
                    const newPath = path.join(dest, files[i]);
                    if ((dest === null || dest === void 0 ? void 0 : (_dest_toLowerCase = dest.toLowerCase()) === null || _dest_toLowerCase === void 0 ? void 0 : _dest_toLowerCase.indexOf(filePath === null || filePath === void 0 ? void 0 : filePath.toLowerCase())) === 0) {
                        // 移动到子目录，排除当前目录
                        // src: 'd:\\123', dest: 'd:\\123\\456\\789 针对这样的目录，不移动456及其子目录（还有一种是不移动456\789，后续可根据需要调整)
                        ret = true;
                        continue;
                    }
                    ret = await existsAW(filePath);
                    if (ret) {
                        stats = await lstatAW(filePath);
                        if (stats) {
                            if (stats.isDirectory()) {
                                ret = await copyDirsAW(filePath, newPath);
                                if (ret) {
                                    await rmdirsAW(filePath);
                                }
                            } else {
                                ret = await copyFileAW(filePath, newPath);
                                if (ret) {
                                    await unlinkAW(filePath);
                                }
                            }
                        }
                    }
                    if (!ret) {
                        break;
                    }
                }
            }
        }
        return ret;
    }
    FileSystemAWNS.moveDirsAW = moveDirsAW;
    async function mkdtempAW() {
        let ret = false;
        const mktemp = promisify(fs.mkdtemp);
        const os = await Promise.resolve(/* import() */).then(__webpack_require__.t.bind(__webpack_require__, 2037, 23));
        const tmpDir = os.tmpdir();
        try {
            ret = await mktemp(`${tmpDir}${path.sep}`);
        } catch (error) {
            console.error(error);
        }
        return ret;
    }
    /**
   * @description 在temp目录下生成一个唯一的临时目录
   * fs.mkdtemp() 方法会直接附加六位随机选择的字符串到 prefix 字符串。
   * 例如，指定一个目录 /tmp，如果目的是要在 /tmp 里创建一个临时目录，
   * 则 prefix 必须 以一个指定平台的路径分隔符
   */ FileSystemAWNS.mkdtempAW = mkdtempAW;
    async function deleteEmptySubDirs(subPath, rootPath) {
        let ret = true;
        subPath = path.normalize(subPath);
        rootPath = path.normalize(rootPath);
        if (subPath.length > 3 && subPath[subPath.length - 1] === '\\') {
            subPath = subPath.slice(0, subPath.length - 1);
        }
        if (rootPath.length > 3 && rootPath[rootPath.length - 1] === '\\') {
            rootPath = rootPath.slice(0, rootPath.length - 1);
        }
        do {
            if (subPath.indexOf(rootPath) !== 0) {
                ret = false;
                break;
            }
            let recursion = subPath;
            while(recursion !== rootPath){
                if (await existsAW(recursion)) {
                    if (!await rmdirAW(recursion)) {
                        ret = false;
                        break;
                    }
                }
                recursion = path.dirname(recursion);
            }
        }while (0);
        return ret;
    }
    /**
   * 删除非空的子目录
   * 例如 E:\迅雷下载\我的应用\123 删除 我的应用\123
   * deleteEmptySubDirs('E:\迅雷下载\我的应用\123', 'E:\迅雷下载\');
   */ FileSystemAWNS.deleteEmptySubDirs = deleteEmptySubDirs;
    async function getFileSize(filePath) {
        let ret = 0;
        do {
            if (!filePath) {
                break;
            }
            if (!await existsAW(filePath)) {
                break;
            }
            const stats = await lstatAW(filePath);
            if (stats) {
                if (stats.isDirectory()) {
                    const files = await readdirAW(filePath);
                    for(let i = 0; i < files.length; i++){
                        const subFile = path.join(filePath, files[i]);
                        ret += await getFileSize(subFile);
                    }
                } else {
                    ret = stats.size;
                }
            }
        }while (0);
        return ret;
    }
    /**
   * @description 获取文件或者文件夹大小
   */ FileSystemAWNS.getFileSize = getFileSize;
    async function isDirectoryEmptyAW(filePath, retValIfNoExist = true) {
        let ret = true;
        do {
            if (!filePath) {
                ret = false;
                break;
            }
            if (!await existsAW(filePath)) {
                ret = retValIfNoExist;
                break;
            }
            const stats = await lstatAW(filePath);
            if (stats) {
                if (stats.isDirectory()) {
                    const files = await readdirAW(filePath);
                    // logger.information('is directory empty', ...files);
                    if (files.length > 0) {
                        ret = false;
                        break;
                    }
                } else {
                    ret = false;
                    break;
                }
            } else {
                ret = false;
                break;
            }
        }while (0);
        return ret;
    }
    /**
   * @description: 判断文件夹是否温控
   * @param filePath 文件路径
   * @param retValIfNoExist 指定 文件/文件夹不存在时的返回值
   * @returns 空文件夹返回true，否则false
   * @example
   * let ret: boolean = await isDirectoryEmptyAW('E:\\迅雷下载');
   */ FileSystemAWNS.isDirectoryEmptyAW = isDirectoryEmptyAW;
    function getValidDirName(str) {
        let result = str;
        if (result) {
            result = result.trim();
            let hasBackslash = false;
            if (result.length > 3) {
                if (result.endsWith('\\')) {
                    hasBackslash = true;
                    // 去掉末尾的\
                    result = result.replace(/(\\+$)/, '');
                    if (result.length < 3) {
                        result = result + '\\';
                    }
                }
            }
            result = result.replace(/(\.+$)/, '');
            if (hasBackslash && !result.endsWith('\\')) {
                // 保留输入格式
                result = result + '\\';
            }
        }
        return result;
    }
    // 去掉目录路径 or 子目录的前后空格和末尾的.
    FileSystemAWNS.getValidDirName = getValidDirName;
    function getFileDir(filePath) {
        let result = '';
        if (filePath) {
            filePath = filePath.trim();
            if (filePath.length > 3) {
                if (filePath.endsWith('\\')) {
                    // 去掉末尾的\
                    filePath = filePath.replace(/(\\+$)/, '');
                }
            }
            result = path.dirname(filePath);
        }
        return result;
    }
    // 获取文件所在目录
    FileSystemAWNS.getFileDir = getFileDir;
    async function checkWritePermission(dirPath) {
        console.log('checkWritePermission', dirPath);
        const testFile = path.join(dirPath, `write_test_${Date.now()}.tmp`);
        try {
            // 尝试写入临时文件
            await fs.promises.writeFile(testFile, 'permission test');
            // 清理测试文件
            await fs.promises.unlink(testFile);
            return true;
        } catch (error) {
            if (error.code === 'EACCES') {
                console.error('权限被拒绝');
            } else {
                console.error('写入失败:', error.message);
            }
            return false;
        }
    }
    /**
   * @description: 判断文件夹是否有写权限
   * @param dirPath  文件夹路径
   * @returns 空文件夹返回true，否则false
   * @example
   * let ret: boolean = await checkWritePermission('E:\\迅雷下载');
   */ FileSystemAWNS.checkWritePermission = checkWritePermission;
    async function isFileReadPermission(filePath) {
        try {
            await fs.promises.access(filePath, fs.constants.R_OK);
            return true;
        } catch (error) {
            return false;
        }
    }
    // 检测文件是否有读权限
    FileSystemAWNS.isFileReadPermission = isFileReadPermission;
})(fs_utilities_FileSystemAWNS || (fs_utilities_FileSystemAWNS = {}));
var fs_utilities_FileSystemAWNS;

// EXTERNAL MODULE: external "path"
var external_path_ = __webpack_require__(1017);
var external_path_default = /*#__PURE__*/__webpack_require__.n(external_path_);
// EXTERNAL MODULE: external "fs"
var external_fs_ = __webpack_require__(7147);
var external_fs_default = /*#__PURE__*/__webpack_require__.n(external_fs_);
// EXTERNAL MODULE: ../player-comp/ui/store/storage/ConfigStore.ts + 1 modules
var ConfigStore = __webpack_require__(8849);
;// CONCATENATED MODULE: ../renderer-common/store/processStore/xmpSettingConfigStore.ts




const xmpSettingConfigStore = new ConfigStore/* ConfigStore */.J({
    name: 'XmpSettingConfig'
});
let macCachePath = '';
if (process.platform === 'darwin') {
    macCachePath = external_path_default().join(process.env.HOME, 'Library', 'Application Support', 'XLPlayer', 'cache');
    try {
        if (!external_fs_default().existsSync(macCachePath)) {
            external_fs_default().mkdirSync(macCachePath, {
                recursive: true
            });
        }
    } catch (e) {}
}
async function getXmpCachePath() {
    if (process.platform === 'darwin') {
        return macCachePath;
    }
    let cachePath = await xmpSettingConfigStore.get('CacheFilePaths') || '';
    if (cachePath.length === 0) {
        cachePath = 'C:/XmpCache';
    }
    xmpSettingConfigStore.set('CacheFilePaths', cachePath);
    if (!await fs_utilities_FileSystemAWNS.existsAW(cachePath)) {
        await fs_utilities_FileSystemAWNS.mkdirsAW(cachePath);
    }
    return cachePath;
}


}),

};
;
//# sourceMappingURL=634.js.map