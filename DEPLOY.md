# AWS Deployment Guide — Amazon Bedrock Edition

> Everything runs inside AWS. No external API keys. User scans QR → speaks → devices respond.

---

## Architecture

```
Phone (anywhere)
   │  HTTPS
   ▼
AWS EC2  (Node.js server)
   ├── Serves the web page
   ├── Calls Amazon Bedrock (Claude AI)  ← IAM role, no key needed
   └── WebSocket relay
          │  WebSocket (internet)
          ▼
       ESP32 at home  →  Relay  →  Bulb / Fan / Bed Light
```

---

## STEP 1 — Enable Bedrock Model Access

> You must do this FIRST or the server will get "Access Denied".

1. Go to **https://console.aws.amazon.com/bedrock**
2. In the left menu → **Model access**
3. Click **"Manage model access"**
4. Find **Anthropic → Claude 3 Haiku** → tick the checkbox
5. Click **"Save changes"**
6. Wait 1–2 minutes until status shows **"Access granted"**

---

## STEP 2 — Create IAM Role for EC2

> This lets your EC2 server call Bedrock without any API key.

1. Go to **https://console.aws.amazon.com/iam**
2. Left menu → **Roles** → **Create role**
3. Select **"AWS service"** → **EC2** → Next
4. Search for **`AmazonBedrockFullAccess`** → tick it → Next
5. Role name: `SmartHomeAI-EC2-Role` → **Create role**

---

## STEP 3 — Launch EC2 Instance

1. Go to **https://console.aws.amazon.com/ec2** → **Launch Instance**
2. Set these options:

| Setting | Value |
|---------|-------|
| Name | SmartHomeAI |
| AMI | Ubuntu Server 22.04 LTS |
| Instance type | **t2.micro** (free tier) |
| Key pair | Create new → `smarthome-key` → Download .pem |
| IAM instance profile | **SmartHomeAI-EC2-Role** ← important! |
| Security group | Allow: SSH (22), HTTP (80), Custom TCP 3000 |

3. Click **Launch Instance**

---

## STEP 4 — Get a Static IP

1. EC2 left menu → **Elastic IPs** → **Allocate Elastic IP**
2. Select it → **Actions → Associate Elastic IP** → choose your instance
3. Note your IP (e.g., `54.123.45.67`) — this never changes

---

## STEP 5 — Upload Your Code

### On Windows — download WinSCP (free, easy GUI)
1. Download from https://winscp.net
2. Open WinSCP → New Session:
   - Protocol: SFTP
   - Host: `YOUR_ELASTIC_IP`
   - User: `ubuntu`
   - Advanced → SSH → Authentication → Private key → select your `.pem` file
3. Connect → drag the **entire `aws-server` folder** to `/home/ubuntu/smarthomeai`

### Alternative — Windows Terminal / PowerShell
```powershell
scp -i "smarthome-key.pem" -r "aws-server" ubuntu@YOUR_IP:~/smarthomeai
```

---

## STEP 6 — Connect to Server and Run Setup

### Connect via SSH
**Option A — AWS Console (no software needed)**
1. Go to EC2 → select your instance → **Connect** → **EC2 Instance Connect** → Connect
2. A terminal opens in your browser ✓

**Option B — Windows Terminal**
```bash
ssh -i "smarthome-key.pem" ubuntu@YOUR_ELASTIC_IP
```

### Run the automated setup script
```bash
cd ~/smarthomeai
bash setup.sh
```

This automatically installs Node.js, PM2, Nginx and starts your server.

---

## STEP 7 — Check It Works

Open in your browser:
```
http://YOUR_ELASTIC_IP/room/HOME001
```

You should see the SmartHome AI page. ✓

Check server health:
```
http://YOUR_ELASTIC_IP/health
```

Should return:
```json
{"status":"ok","engine":"Amazon Bedrock","model":"anthropic.claude-3-haiku..."}
```

---

## STEP 8 — Configure ESP32

Open `SmartHomeAI/SmartHomeAI.ino`, update these 3 lines:

```cpp
#define WIFI_SSID      "Prem"
#define WIFI_PASSWORD  "premprem"
#define SERVER_HOST    "54.123.45.67"   // ← your Elastic IP
#define SERVER_PORT    80
#define USE_SSL        false
#define ROOM_ID        "HOME001"
```

Upload to ESP32. Serial Monitor should show:
```
[WIFI] Connected ✓
[WS] Connected to AWS server
```

---

## STEP 9 — Generate QR Code

```bash
python generate_qr.py http://YOUR_ELASTIC_IP/room/HOME001
```

Print and stick on the wall. Done!

---

## STEP 10 — HTTPS (for mic on internet, optional)

The microphone works fine when the phone is on home WiFi (HTTP is OK locally).
For full internet access from anywhere, you need HTTPS:

1. Get a free subdomain from **https://duckdns.org**
   - Sign in with Google → create e.g. `mysmarthome` → point to your Elastic IP
2. On your EC2 server:
```bash
sudo apt install -y certbot python3-certbot-nginx
sudo certbot --nginx -d mysmarthome.duckdns.org
```
3. Update your Nginx config `server_name` to your domain
4. Your URL becomes: `https://mysmarthome.duckdns.org/room/HOME001`
5. Update ESP32: `SERVER_PORT=443`, `USE_SSL=true`

---

## Useful Commands

```bash
pm2 status                  # is server running?
pm2 logs smarthomeai        # live logs
pm2 restart smarthomeai     # restart after code changes
pm2 stop smarthomeai        # stop server

# Update code (after uploading new files)
cd ~/smarthomeai && pm2 restart smarthomeai
```

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| "AccessDeniedException" | IAM role not attached to EC2, or Bedrock model not enabled |
| "ValidationException" | Go to Bedrock console → Model access → Enable Claude 3 Haiku |
| Page not loading | Security group: open port 80. Check `pm2 status` |
| ESP32 not connecting | Check `SERVER_HOST` in firmware. Check Security group port 3000 or 80 |
| Mic blocked | Need HTTPS — do Step 10 |
| "Model not found" | Change region in `.env` to where you enabled Bedrock access |
