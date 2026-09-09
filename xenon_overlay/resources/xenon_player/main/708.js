"use strict";
exports.ids = ['708'];
exports.modules = {
8571: (function (__unused_webpack_module, __webpack_exports__, __webpack_require__) {
// ESM COMPAT FLAG
__webpack_require__.r(__webpack_exports__);

// EXPORTS
__webpack_require__.d(__webpack_exports__, {
  playerUserDataStore: () => (/* binding */ playerUserDataStore)
});

// EXTERNAL MODULE: ../player-comp/ui/store/db/jsonFileDb.ts
var jsonFileDb = __webpack_require__(3781);
// EXTERNAL MODULE: ../player-comp/ui/store/storage/Store.ts
var Store = __webpack_require__(9405);
// EXTERNAL MODULE: ../player-comp/ui/utils/checkTypes.ts
var checkTypes = __webpack_require__(7921);
// EXTERNAL MODULE: ../player-comp/common/constant.ts
var constant = __webpack_require__(8543);
// EXTERNAL MODULE: ../../node_modules/.pnpm/@xunlei+node-net-ipc@1.0.32/node_modules/@xunlei/node-net-ipc/dist/ipc-client.js
var ipc_client = __webpack_require__(9257);
;// CONCATENATED MODULE: ../player-comp/ui/store/storage/UserDataStore.ts




// 用户数据缓存，基于 UserID 进行数据隔离
// 非响应式, 应用于 主进程和渲染进程 共享数据
class UserDataStore extends Store/* Store */.y {
    _userInfo;
    constructor(options){
        super(options);
        this._init();
    }
    _init() {
        ipc_client.client.on(constant/* Emitter_Channel,OnUpdateUserInfo */.$g.OnUpdateUserInfo, (info)=>{
            this._userInfo = info;
        });
    }
    getUid() {
        if (this._userInfo) {
            return this._userInfo.sub;
        }
        return '';
    }
    async setViaUid(key, value, uid) {
        if ((0,checkTypes/* isNil */.kK)(uid)) {
            uid = this.getUid();
        }
        if (uid) {
            const data = await this.get(String(key)) || Object.create({});
            const nextData = {
                ...data,
                [uid]: {
                    ...data[uid],
                    ...value
                }
            };
            await super.set(String(key), nextData);
        }
    }
    async getViaUid(key, defaultValue, uid) {
        if ((0,checkTypes/* isNil */.kK)(uid)) {
            uid = this.getUid();
        }
        if (uid) {
            const data = await super.get(String(key));
            return (data === null || data === void 0 ? void 0 : data[uid]) ?? defaultValue;
        }
        return undefined;
    }
    async deleteViaUid(key, uid) {
        if ((0,checkTypes/* isNil */.kK)(key)) {
            return;
        }
        if ((0,checkTypes/* isNil */.kK)(uid)) {
            uid = await this.getUid();
        }
        if (uid) {
            const data = await this.getViaUid(key) || Object.create({});
            try {
                delete data[uid];
            } catch (e) {}
            await super.set(String(key), {
                ...data
            });
        }
    }
}

// EXTERNAL MODULE: ../player-comp/common/env.ts
var env = __webpack_require__(1598);
;// CONCATENATED MODULE: ../player-comp/ui/store/processStore/playerUserDataStore.ts



const playerUserDataStore = new UserDataStore({
    filePath: jsonFileDb/* JsonDbPath,PlayerCacheData */.J.PlayerCacheData,
    name: 'playerUserData'
});
const playerUserDataStoreOld = new UserDataStore({
    filePath: jsonFileDb/* JsonDbPath,AppCacheData */.J.AppCacheData,
    name: 'playerUserData'
});
if (env/* isProcess */.Hm) {
    // 迁移旧配置到新配置
    ;
    (async ()=>{
        const allValue = await playerUserDataStore.getAllValue();
        if (!allValue) {
            const oldAllValue = await playerUserDataStoreOld.getAllValue();
            if (oldAllValue) {
                for (const key of Object.keys(oldAllValue)){
                    // @ts-ignore
                    await playerUserDataStore.set(key, oldAllValue[key]);
                }
                console.log('playerUserData remove old');
                await playerUserDataStoreOld.delete();
            }
        }
    })();
}


}),

};
;
//# sourceMappingURL=708.js.map