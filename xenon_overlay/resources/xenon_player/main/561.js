"use strict";
exports.ids = ['561'];
exports.modules = {
6320: (function (__unused_webpack_module, __webpack_exports__, __webpack_require__) {
__webpack_require__.r(__webpack_exports__);
__webpack_require__.d(__webpack_exports__, {
  AppNewNoticeKey: () => (AppNewNoticeKey),
  AppNewNoticeKey_List: () => (AppNewNoticeKey_List),
  appHistoryStore: () => (appHistoryStore),
  appNewNoticeStore: () => (appNewNoticeStore)
});
/* ESM import */var _root_player_comp_ui_store_db_jsonFileDb__WEBPACK_IMPORTED_MODULE_0__ = __webpack_require__(3781);
/* ESM import */var _root_player_comp_ui_store_storage_Store__WEBPACK_IMPORTED_MODULE_1__ = __webpack_require__(9405);


const appHistoryStore = new _root_player_comp_ui_store_storage_Store__WEBPACK_IMPORTED_MODULE_1__/* .Store */.y({
    filePath: _root_player_comp_ui_store_db_jsonFileDb__WEBPACK_IMPORTED_MODULE_0__/* .JsonDbPath.AppCacheData */.J.AppCacheData,
    name: 'history'
});
// 命名规则 page_module_key
var AppNewNoticeKey = /*#__PURE__*/ function(AppNewNoticeKey) {
    AppNewNoticeKey["sidebar_file_thridPanBindSource"] = "sidebar_file_thridPanBindSource";
    AppNewNoticeKey["file_title_addThridPanBindSource"] = "file_title_addThridPanBindSource";
    AppNewNoticeKey["sidebar_mediaLibrary_noEnableScrape"] = "sidebar_mediaLibrary_noEnableScrape";
    return AppNewNoticeKey;
}({});
const AppNewNoticeKey_List = Object.values(AppNewNoticeKey);
const appNewNoticeStore = new _root_player_comp_ui_store_storage_Store__WEBPACK_IMPORTED_MODULE_1__/* .Store */.y({
    filePath: _root_player_comp_ui_store_db_jsonFileDb__WEBPACK_IMPORTED_MODULE_0__/* .JsonDbPath.AppCacheData */.J.AppCacheData,
    name: 'newNotice'
});


}),

};
;
//# sourceMappingURL=561.js.map