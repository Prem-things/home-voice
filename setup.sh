#!/bin/bash
# =====================================================
#  SmartHomeAI — Auto Setup Script for AWS EC2
#  Run this ONCE on your EC2 instance:
#  bash setup.sh
# =====================================================
set -e

echo ""
echo "======================================"
echo "  SmartHomeAI — AWS Setup Starting"
echo "======================================"

# ── Install Node.js 20 ─────────────────────────────
echo ""
echo "[1/5] Installing Node.js..."
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash - > /dev/null 2>&1
sudo apt-get install -y nodejs > /dev/null 2>&1
echo "      Node.js $(node --version) installed ✓"

# ── Install PM2 ────────────────────────────────────
echo "[2/5] Installing PM2..."
sudo npm install -g pm2 > /dev/null 2>&1
echo "      PM2 installed ✓"

# ── Install app dependencies ───────────────────────
echo "[3/5] Installing app dependencies..."
cd ~/smarthomeai
npm install > /dev/null 2>&1
echo "      Dependencies installed ✓"

# ── Setup Nginx ────────────────────────────────────
echo "[4/5] Setting up Nginx..."
sudo apt-get install -y nginx > /dev/null 2>&1

sudo tee /etc/nginx/sites-available/smarthomeai > /dev/null <<'NGINXCONF'
server {
    listen 80;
    server_name _;

    location / {
        proxy_pass         http://localhost:3000;
        proxy_http_version 1.1;
        proxy_set_header   Upgrade    $http_upgrade;
        proxy_set_header   Connection "upgrade";
        proxy_set_header   Host       $host;
        proxy_set_header   X-Real-IP  $remote_addr;
        proxy_read_timeout 86400;
    }
}
NGINXCONF

sudo ln -sf /etc/nginx/sites-available/smarthomeai /etc/nginx/sites-enabled/
sudo rm -f /etc/nginx/sites-enabled/default
sudo nginx -t > /dev/null 2>&1 && sudo systemctl restart nginx
echo "      Nginx configured ✓"

# ── Start app with PM2 ─────────────────────────────
echo "[5/5] Starting SmartHomeAI server..."
pm2 delete smarthomeai 2>/dev/null || true
pm2 start server.js --name smarthomeai
pm2 startup 2>/dev/null | grep "sudo" | sudo bash > /dev/null 2>&1 || true
pm2 save > /dev/null 2>&1

# ── Done ───────────────────────────────────────────
PUBLIC_IP=$(curl -s ifconfig.me 2>/dev/null || echo "YOUR_IP")

echo ""
echo "======================================"
echo "  DONE! Server is live!"
echo "======================================"
echo ""
echo "  Open this URL on your phone:"
echo "  http://$PUBLIC_IP/room/HOME001"
echo ""
echo "  QR code URL (run on your PC):"
echo "  python generate_qr.py http://$PUBLIC_IP/room/HOME001"
echo ""
echo "  Check logs anytime:"
echo "  pm2 logs smarthomeai"
echo "======================================"
