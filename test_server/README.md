# 本機測試用上傳伺服器

給 Pico 中繼裝置的 `upload_api.c` 測試上傳用，純 Python 標準函式庫，不需要 `pip install`。

```
python app.py
```

啟動後：
- 瀏覽器打開 `http://127.0.0.1:5000/` 可以看到收到的資料（每 5 秒自動重新整理）。
- Pico 要打的上傳網址是 `http://<這台電腦的區域網路IP>:5000/api/vitals`（跟 Pico 要在同一個 WiFi 網段）。

## API 規格（暫定，測試用）

```
POST /api/vitals
Content-Type: application/json

{
  "patient_id": "個案編號字串",
  "readings": [
    { "type": 1, "value": 37.0, "received_at_ms": 123456 },
    ...
  ]
}
```

- `type`：對應 `common.h` 的 `vital_type_t`（1=體溫、2=血氧、3=脈搏）。
- 回應 `200 {"status":"ok"}` 代表成功。

這只是測試用的暫定格式，不是正式 API 規格；正式對接時要換成真實的 endpoint/認證方式/payload 格式。
