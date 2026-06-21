import { initializeApp } from "https://www.gstatic.com/firebasejs/10.12.2/firebase-app.js";

import {
getDatabase,
ref,
set,
get,
onValue
} from "https://www.gstatic.com/firebasejs/10.12.2/firebase-database.js";

/* =====================================================
FIREBASE CONFIG
===================================================== */

const firebaseConfig = {
apiKey: "AIzaSyD8GPY7DoFimhKYqVRCjnbjV3QUT6y1KLM",
authDomain: "starm-vdk.firebaseapp.com",
databaseURL: "https://starm-vdk-default-rtdb.asia-southeast1.firebasedatabase.app",
projectId: "starm-vdk",
storageBucket: "starm-vdk.firebasestorage.app",
messagingSenderId: "895942402341",
appId: "1:895942402341:web:396610bc92225b076dfacc"
};

const app = initializeApp(firebaseConfig);
const database = getDatabase(app);

/* =====================================================
BIỂU ĐỒ
===================================================== */

let chart;

let labels = [];
let tempData = [];
let humidityData = [];
let soilData = [];

/* =====================================================
TRẠNG THÁI HỆ THỐNG
===================================================== */

let currentMode = "MANUAL";

let smartSettings = {
version: 0,

   
sunny: {
    temp: 32,
    humidity: 60,
    soil: 20
},

rain: {
    temp: 30,
    humidity: 70,
    soil: 70
}
   

};

let settingsVersion = 0;

let lightTimer = {
enabled: false,
onTime: "",
offTime: ""
};

/* =====================================================
LỆNH ĐANG CHỜ ATMEGA XÁC NHẬN

Mục đích:

* Khi web vừa gửi lệnh MỞ servo = 1.
* Firebase sensor có thể vẫn còn servoReal = 0 cũ.
* Không cho servoReal cũ làm công tắc nhảy về ĐÓNG.
  ===================================================== */

let pendingCommand = {
pump: {
active: false,
target: 0,
startedAt: 0
},

   
servo: {
    active: false,
    target: 0,
    startedAt: 0
},

light: {
    active: false,
    target: 0,
    startedAt: 0
}
   

};

const COMMAND_CONFIRM_TIMEOUT_MS = 5000;

/* =====================================================
KHỞI ĐỘNG WEB
===================================================== */

document.addEventListener("DOMContentLoaded", function () {
initChart();
listenControlData();
listenSensorData();
listenSmartSettings();
listenLightTimer();
});

/* =====================================================
ĐIỀU KHIỂN THIẾT BỊ
===================================================== */

window.toggleDevice = function (device, checked) {

   
/*
   Nếu đèn đang chạy theo hẹn giờ,
   không cho điều khiển tay.
*/
if (device === "light" && lightTimer.enabled) {

    setText(
        "controlStatus",
        "⚠ Đèn đang ở chế độ hẹn giờ, hãy tắt Set time nếu muốn bật/tắt tay"
    );

    const switchEl = document.getElementById("lightSwitch");

    if (switchEl) {
        switchEl.checked = !checked;
    }

    return;
}


/*
   Trong AUTO:
   - Không cho điều khiển bơm và servo bằng tay.
   - Đèn vẫn hoạt động độc lập.
*/
if (currentMode === "AUTO" && device !== "light") {

    setText(
        "controlStatus",
        "⚠ Đang ở chế độ AUTO, không thể điều khiển tay"
    );

    const switchEl =
        document.getElementById(device + "Switch");

    if (switchEl) {
        switchEl.checked = !checked;
    }

    return;
}


const value =
    device === "servo"
        ? (checked ? 0 : 1)   // Servo: checked=MỞ=0, unchecked=ĐÓNG=1
        : (checked ? 1 : 0);  // Bơm, đèn: checked=ON=1


/*
   Bắt đầu chờ trạng thái thực từ ATmega.
*/
beginPendingCommand(device, value);


/*
   Cập nhật giao diện ngay lập tức.
*/
updateDeviceUI(device, value);


/*
   Gửi lệnh lên Firebase.
*/
set(
    ref(database, "smartfarm/control/" + device),
    value
)
    .then(function () {

        setText(
            "controlStatus",
            "✅ Đã gửi lệnh " +
            getDeviceName(device) +
            ": " +
            getStateText(device, value) +
            ""
        );

    })
    .catch(function (error) {

        cancelPendingCommand(device);

        setText(
            "controlStatus",
            "❌ Lỗi gửi lệnh: " + error.message
        );

        /*
           Firebase ghi thất bại:
           trả công tắc về trạng thái trước.
        */
        const previousValue =
            device === "servo"
                ? (checked ? 1 : 0)
                : (checked ? 0 : 1);

        updateDeviceUI(
            device,
            previousValue
        );

    });
   

};

/* =====================================================
CHUYỂN AUTO / MANUAL
===================================================== */

window.setMode = function (mode) {

   
if (mode !== "AUTO" && mode !== "MANUAL") {
    return;
}

const previousMode = currentMode;


/*
   Khi đổi chế độ:
   hủy lệnh tay của servo và bơm đang chờ.

   Đèn độc lập AUTO/MANUAL nên không hủy.
*/
cancelPendingCommand("pump");
cancelPendingCommand("servo");


/*
   Cập nhật giao diện trước.
*/
updateModeUI(mode);


/*
   Ghi mode lên Firebase.
*/
set(
    ref(database, "smartfarm/control/mode"),
    mode
)
    .then(function () {

        setText(
            "controlStatus",
            "✅ Đã chuyển chế độ: " + mode
        );

    })
    .catch(function (error) {

        /*
           Nếu ghi Firebase lỗi,
           trả giao diện về mode cũ.
        */
        updateModeUI(previousMode);

        setText(
            "controlStatus",
            "❌ Lỗi chuyển chế độ: " + error.message
        );

    });
   

};

/* =====================================================
LẮNG NGHE CONTROL FIREBASE
===================================================== */

function listenControlData() {

   
/*
   MODE
*/
onValue(
    ref(database, "smartfarm/control/mode"),
    function (snapshot) {

        const mode = snapshot.val();

        if (mode === "AUTO" || mode === "MANUAL") {

            updateModeUI(mode);

        } else {

            set(
                ref(database, "smartfarm/control/mode"),
                "MANUAL"
            );

            updateModeUI("MANUAL");
        }
    }
);


/*
   PUMP COMMAND
   Đây là trạng thái lệnh trên Firebase,
   chưa phải trạng thái thật từ ATmega.
*/
onValue(
    ref(database, "smartfarm/control/pump"),
    function (snapshot) {

        const value = snapshot.val();

        if (!hasPendingCommand("pump")) {
            updateDeviceUI("pump", value);
        }
    }
);


/*
   LIGHT COMMAND
*/
onValue(
    ref(database, "smartfarm/control/light"),
    function (snapshot) {

        const value = snapshot.val();

        if (!hasPendingCommand("light")) {
            updateDeviceUI("light", value);
        }
    }
);


/*
   SERVO COMMAND
*/
onValue(
    ref(database, "smartfarm/control/servo"),
    function (snapshot) {

        const value = snapshot.val();

        if (!hasPendingCommand("servo")) {
            updateDeviceUI("servo", value);
        }
    }
);
   

}

/* =====================================================
LẮNG NGHE DỮ LIỆU SENSOR VÀ TRẠNG THÁI THẬT
===================================================== */

function listenSensorData() {

   
const sensorRef =
    ref(database, "smartfarm/sensor");


onValue(
    sensorRef,

    function (snapshot) {

        const data = snapshot.val();


        if (!data) {

            setText(
                "connectionStatus",
                "⚠ Chưa có dữ liệu cảm biến"
            );

            setText(
                "realtimeValue",
                "--"
            );

            setText(
                "realtimeStatus",
                "Chưa có dữ liệu realtime"
            );

            return;
        }


        const temp =
            toNumber(data.temperature);

        const humidity =
            toNumber(data.humidity);

        const soil =
            toNumber(data.soil);

        const status =
            data.status || "UNKNOWN";


        /*
           Cập nhật giá trị cảm biến.
        */
        setText(
            "temp",
            temp + " °C"
        );

        setText(
            "humidity",
            humidity + " %"
        );

        setText(
            "soil",
            soil + " %"
        );

        setText(
            "connectionStatus",
            "🟢 Đã kết nối Firebase"
        );


        updateRealtimeBox(status);

        updateSensorStatus(
            temp,
            humidity,
            soil,
            status
        );

        updateAISuggestion(
            temp,
            humidity,
            soil,
            status
        );

        updateUART(
            temp,
            humidity,
            soil,
            status,
            data
        );

        updateChart(
            temp,
            humidity,
            soil
        );


        /*
           Tuyệt đối không gọi:

           updateModeUI(data.mode)

           Mode của giao diện phải lấy từ:

           smartfarm/control/mode

           Nếu lấy mode trong sensor,
           giao diện có thể tự nhảy AUTO/MANUAL.
        */


        /*
           Trạng thái thật máy bơm từ ATmega.
        */
        if (data.pumpReal !== undefined) {

            handleRealDeviceState(
                "pump",
                data.pumpReal
            );
        }


        /*
           Trạng thái thật servo từ ATmega.

           Khi đang chờ MỞ = 1:
           nếu nhận servoReal = 0 cũ,
           hàm sẽ bỏ qua nên giao diện không nhảy ĐÓNG.
        */
        if (data.servoReal !== undefined) {

            handleRealDeviceState(
                "servo",
                data.servoReal
            );
        }


        /*
           Trạng thái thật đèn từ ATmega.
        */
        if (data.lightReal !== undefined) {

            handleRealDeviceState(
                "light",
                data.lightReal
            );
        }

    },

    function (error) {

        setText(
            "connectionStatus",
            "🔴 Lỗi Firebase: " + error.message
        );

        setText(
            "realtimeValue",
            "--"
        );

        setText(
            "realtimeStatus",
            "Lỗi đồng bộ Firebase"
        );
    }
);
   

}

/* =====================================================
BẮT ĐẦU CHỜ LỆNH XÁC NHẬN
===================================================== */

function beginPendingCommand(device, target) {

   
const item = pendingCommand[device];

if (!item) {
    return;
}


const startedAt = Date.now();


item.active = true;

item.target =
    Number(target) === 1 ? 1 : 0;

item.startedAt =
    startedAt;


/*
   Nếu sau 5 giây vẫn chưa nhận được
   trạng thái thực đúng từ ATmega:

   - Dừng chờ.
   - Không khóa giao diện vĩnh viễn.
   - Báo cho người dùng.
*/
setTimeout(function () {

    const currentItem =
        pendingCommand[device];


    if (
        currentItem &&
        currentItem.active &&
        currentItem.startedAt === startedAt
    ) {

        currentItem.active = false;
        currentItem.startedAt = 0;


        setText(
            "controlStatus",
            "⚠ Đã gửi lệnh " +
            getDeviceName(device) +
            ""
        );
    }

}, COMMAND_CONFIRM_TIMEOUT_MS);
   

}

/* =====================================================
HỦY LỆNH ĐANG CHỜ
===================================================== */

function cancelPendingCommand(device) {

   
const item =
    pendingCommand[device];

if (!item) {
    return;
}

item.active = false;
item.startedAt = 0;
   

}

/* =====================================================
KIỂM TRA CÓ LỆNH ĐANG CHỜ KHÔNG
===================================================== */

function hasPendingCommand(device) {

   
const item =
    pendingCommand[device];

return Boolean(
    item &&
    item.active
);
   

}

/* =====================================================
NHẬN TRẠNG THÁI THẬT TỪ ATMEGA
===================================================== */

function handleRealDeviceState(device, value) {

   
const item =
    pendingCommand[device];

const realValue =
    Number(value) === 1 ? 1 : 0;


if (!item) {
    return;
}


/*
   Không có lệnh đang chờ:
   cập nhật trạng thái thật bình thường.

   Không hiện thông báo xác nhận lại liên tục.
*/
if (!item.active) {

    updateDeviceUI(
        device,
        realValue
    );

    return;
}


/*
   Có lệnh đang chờ nhưng trạng thái thực
   chưa trùng với trạng thái yêu cầu.

   Ví dụ:
   - Web vừa yêu cầu MỞ = 1.
   - Firebase sensor vẫn còn servoReal = 0 cũ.

   Bỏ qua trạng thái cũ,
   không cho giao diện nhảy về ĐÓNG.
*/
if (realValue !== item.target) {
    return;
}


/*
   ATmega đã trả đúng trạng thái yêu cầu.
*/
item.active = false;
item.startedAt = 0;


updateDeviceUI(
    device,
    realValue
);


setText(
    "controlStatus",
    "✅ ATmega đã xác nhận " +
    getDeviceName(device) +
    ": " +
    getStateText(device, realValue)
);
   

}

/* =====================================================
CẬP NHẬT GIAO DIỆN THIẾT BỊ
===================================================== */

function updateDeviceUI(device, value) {

    const normalizedValue =
        Number(value) === 1 ? 1 : 0;

    /*
       Quy ước:
       - Servo / mái che: 0 = MỞ, 1 = ĐÓNG
       - Bơm, đèn: 1 = ON, 0 = OFF
    */
    const isOn =
        device === "servo"
            ? normalizedValue === 0
            : normalizedValue === 1;

    const switchEl =
        document.getElementById(
            device + "Switch"
        );

    const stateEl =
        document.getElementById(
            device + "State"
        );

    if (switchEl) {
        switchEl.checked = isOn;
    }

    if (!stateEl) {
        return;
    }

    /*
       Trong AUTO:
       - Bơm và servo hiển thị AUTO.
       - Đèn vẫn hiển thị ON/OFF.
    */
    if (
        currentMode === "AUTO" &&
        device !== "light"
    ) {

        stateEl.innerText = "AUTO";

        stateEl.classList.remove(
            "on-text",
            "off-text"
        );

        stateEl.classList.add(
            "mode-text"
        );

        return;
    }

    stateEl.classList.remove(
        "mode-text"
    );

    /*
       Quan trọng:
       Không dùng isOn ? 1 : 0 cho servo,
       vì servo đang quy ước 0 = MỞ, 1 = ĐÓNG.
    */
    stateEl.innerText =
        getStateText(
            device,
            normalizedValue
        );

    stateEl.classList.toggle(
        "on-text",
        isOn
    );

    stateEl.classList.toggle(
        "off-text",
        !isOn
    );
}

/* =====================================================
CẬP NHẬT MODE GIAO DIỆN
===================================================== */

function updateModeUI(mode) {

   
currentMode = mode;


setText(
    "modeState",
    mode
);


const autoBtn =
    document.getElementById("autoBtn");

const manualBtn =
    document.getElementById("manualBtn");


if (autoBtn && manualBtn) {

    autoBtn.classList.toggle(
        "mode-active",
        mode === "AUTO"
    );

    manualBtn.classList.toggle(
        "mode-active",
        mode === "MANUAL"
    );
}


updateControlModeUI();
   

}

/* =====================================================
KHÓA/MỞ CÔNG TẮC THEO MODE
===================================================== */

function updateControlModeUI() {

   
const devices = [
    "pump",
    "light",
    "servo"
];


devices.forEach(function (device) {

    const switchEl =
        document.getElementById(
            device + "Switch"
        );

    const stateEl =
        document.getElementById(
            device + "State"
        );


    if (!switchEl || !stateEl) {
        return;
    }


    if (
        currentMode === "AUTO" &&
        device !== "light"
    ) {

        switchEl.disabled = true;

        stateEl.innerText = "AUTO";

        stateEl.classList.remove(
            "on-text",
            "off-text"
        );

        stateEl.classList.add(
            "mode-text"
        );

    } else {

        if (device === "light") {

            switchEl.disabled =
                lightTimer.enabled;

        } else {

            switchEl.disabled = false;
        }


        stateEl.classList.remove(
            "mode-text"
        );


        const valueFromSwitch =
            device === "servo"
                ? (switchEl.checked ? 0 : 1)
                : (switchEl.checked ? 1 : 0);

        updateDeviceUI(
            device,
            valueFromSwitch
        );
    }
});
   

}

/* =====================================================
REALTIME BOX
===================================================== */

function updateRealtimeBox(status) {

   
const now =
    new Date().toLocaleTimeString();


let text = status;


if (status === "HOT_DRY") {

    text = "☀️ HOT_DRY";

} else if (status === "RAIN") {

    text = "🌧️ RAIN";

} else if (status === "NORMAL") {

    text = "✅ NORMAL";
}


setText(
    "realtimeValue",
    text
);

setText(
    "realtimeStatus",
    "Realtime Firebase • Cập nhật: " + now
);
   

}

/* =====================================================
TRẠNG THÁI CẢM BIẾN
===================================================== */

function updateSensorStatus(
    temp,
    humidity,
    soil,
    status
) {
    const sunny = smartSettings.sunny;
    const rain = smartSettings.rain;

    const currentStatus = String(
        status || "UNKNOWN"
    ).trim().toUpperCase();

    /*
       Ưu tiên trạng thái tổng do ATmega xác định.
       Nhờ vậy RAIN sẽ không còn hiển thị nội dung nắng nóng.
    */
    if (currentStatus === "RAIN") {
        setText(
            "tempStatus",
            "🌧 Hệ thống đang xác định trạng thái mưa"
        );

        setText(
            "humidityStatus",
            "💧 Độ ẩm không khí hiện tại: " +
            humidity +
            " %"
        );

        setText(
            "soilStatus",
            "💧 Độ ẩm đất hiện tại: " +
            soil +
            " % — không cần tưới thêm"
        );

        return;
    }

    if (currentStatus === "HOT_DRY") {
        setText(
            "tempStatus",
            "☀ Hệ thống đang xác định trạng thái nóng và khô"
        );

        setText(
            "humidityStatus",
            "💨 Độ ẩm không khí hiện tại: " +
            humidity +
            " %"
        );

        setText(
            "soilStatus",
            "🌱 Độ ẩm đất hiện tại: " +
            soil +
            " % — cây có nguy cơ thiếu nước"
        );

        return;
    }

    /*
       Khi trạng thái NORMAL,
       chỉ mô tả từng thông số, không tự kết luận là mưa hay nắng.
    */
    if (temp >= sunny.temp) {
        setText(
            "tempStatus",
            "🌡 Nhiệt độ đang cao hơn ngưỡng cài đặt"
        );
    } else if (temp <= rain.temp) {
        setText(
            "tempStatus",
            "🌡 Nhiệt độ đang thấp hơn ngưỡng thông thường"
        );
    } else {
        setText(
            "tempStatus",
            "🌤 Nhiệt độ hiện đang ổn định"
        );
    }

    if (humidity >= rain.humidity) {
        setText(
            "humidityStatus",
            "💧 Độ ẩm không khí đang ở mức cao"
        );
    } else if (humidity <= sunny.humidity) {
        setText(
            "humidityStatus",
            "💨 Độ ẩm không khí đang ở mức thấp"
        );
    } else {
        setText(
            "humidityStatus",
            "🌤 Độ ẩm không khí đang ổn định"
        );
    }

    if (soil <= sunny.soil) {
        setText(
            "soilStatus",
            "🌱 Đất đang khô, cần theo dõi tưới nước"
        );
    } else if (soil >= rain.soil) {
        setText(
            "soilStatus",
            "💧 Đất đang đủ ẩm, chưa cần tưới thêm"
        );
    } else {
        setText(
            "soilStatus",
            "🌿 Độ ẩm đất đang phù hợp"
        );
    }
}

/* =====================================================
GỢI Ý PHÂN TÍCH
===================================================== */

function updateAISuggestion(
    temp,
    humidity,
    soil,
    status
) {
    const currentStatus = String(
        status || "UNKNOWN"
    ).trim().toUpperCase();

    let suggest = "";

    if (currentStatus === "RAIN") {
        suggest =
            "🌧 Hệ thống đang ở trạng thái RAIN theo kết quả tổng hợp từ ATmega. " +
            "Nhiệt độ hiện tại là " +
            temp +
            " °C, độ ẩm không khí " +
            humidity +
            " % và độ ẩm đất " +
            soil +
            " %. " +
            "Độ ẩm đất đang cao nên không cần tưới thêm.";

        setText(
            "aiSuggest",
            suggest
        );

        return;
    }

    if (currentStatus === "HOT_DRY") {
        suggest =
            "☀ Hệ thống đang ở trạng thái HOT_DRY. " +
            "Nhiệt độ hiện tại là " +
            temp +
            " °C, độ ẩm không khí " +
            humidity +
            " % và độ ẩm đất " +
            soil +
            " %. " +
            "Cần theo dõi và bổ sung nước cho cây.";

        setText(
            "aiSuggest",
            suggest
        );

        return;
    }

    if (currentStatus === "NORMAL") {
        suggest =
            "✅ Hệ thống đang ở trạng thái NORMAL. " +
            "Các thông số môi trường chưa đồng thời đạt điều kiện RAIN hoặc HOT_DRY. " +
            "Tiếp tục theo dõi nhiệt độ, độ ẩm không khí và độ ẩm đất.";

        setText(
            "aiSuggest",
            suggest
        );

        return;
    }

    setText(
        "aiSuggest",
        "⚠ Chưa xác định được trạng thái tổng của hệ thống."
    );
}

/* =====================================================
UART MONITOR
===================================================== */

function updateUART(
temp,
humidity,
soil,
status,
data
) {

   
const uartBox =
    document.getElementById("uartData");


if (!uartBox) {
    return;
}


const now =
    new Date().toLocaleTimeString();


const servoReal =
    data.servoReal;


const servoText =
    (
        servoReal === undefined ||
        servoReal === null
    )
        ? "--"
        : getStateText(
            "servo",
            servoReal
        ) +
        " (" +
        servoReal +
        ")";


const text =
   

`[${now}] DATA RECEIVED

TEMP     : ${temp} °C
HUMIDITY : ${humidity} %
SOIL     : ${soil} %

STATUS   : ${status}
MODE REAL: ${data.modeReal || "--"}
MODE WEB : ${currentMode}
PUMP     : ${data.pumpReal ?? "--"}
SERVO    : ${servoText}
LIGHT    : ${data.lightReal ?? "--"}
SYSTEM   : OK

---

`;

   
uartBox.textContent =
    text +
    uartBox.textContent;
   

}

/* =====================================================
KHỞI TẠO BIỂU ĐỒ
===================================================== */

function initChart() {

   
const canvas =
    document.getElementById("sensorChart");


if (
    !canvas ||
    typeof Chart === "undefined"
) {
    return;
}


const ctx =
    canvas.getContext("2d");


chart = new Chart(ctx, {

    type: "line",

    data: {

        labels: labels,

        datasets: [

            {
                label: "Nhiệt độ °C",
                data: tempData,
                borderColor: "#00ffc8",
                backgroundColor: "rgba(0,255,200,0.12)",
                borderWidth: 3,
                tension: 0.4
            },

            {
                label: "Độ ẩm %",
                data: humidityData,
                borderColor: "#00aaff",
                backgroundColor: "rgba(0,170,255,0.12)",
                borderWidth: 3,
                tension: 0.4
            },

            {
                label: "Độ ẩm đất %",
                data: soilData,
                borderColor: "#ffcc00",
                backgroundColor: "rgba(255,204,0,0.12)",
                borderWidth: 3,
                tension: 0.4
            }
        ]
    },

    options: {

        responsive: true,

        maintainAspectRatio: false,

        animation: false,

        plugins: {

            legend: {

                labels: {
                    color: "#ffffff"
                }
            }
        },

        scales: {

            x: {

                ticks: {
                    color: "#9ca3af"
                },

                grid: {
                    color: "rgba(255,255,255,0.08)"
                }
            },

            y: {

                beginAtZero: true,

                ticks: {
                    color: "#9ca3af"
                },

                grid: {
                    color: "rgba(255,255,255,0.08)"
                }
            }
        }
    }
});


window.smartChart = chart;
   

}

/* =====================================================
CẬP NHẬT BIỂU ĐỒ
===================================================== */

function updateChart(
temp,
humidity,
soil
) {

   
if (!chart) {
    return;
}


const time =
    new Date().toLocaleTimeString();


labels.push(time);
tempData.push(temp);
humidityData.push(humidity);
soilData.push(soil);


if (labels.length > 12) {

    labels.shift();
    tempData.shift();
    humidityData.shift();
    soilData.shift();
}


chart.update();
   

}

/* =====================================================
BẬT/TẮT HẸN GIỜ ĐÈN
===================================================== */

window.toggleLightTimer = function () {
saveLightTimer();
};

/* =====================================================
LƯU HẸN GIỜ ĐÈN
===================================================== */

window.saveLightTimer = function () {

   
const enabledEl =
    document.getElementById(
        "lightTimerSwitch"
    );

const onTimeEl =
    document.getElementById(
        "lightOnTime"
    );

const offTimeEl =
    document.getElementById(
        "lightOffTime"
    );


const data = {

    enabled:
        enabledEl
            ? enabledEl.checked
            : false,

    onTime:
        onTimeEl
            ? onTimeEl.value
            : "",

    offTime:
        offTimeEl
            ? offTimeEl.value
            : ""
};


set(
    ref(
        database,
        "smartfarm/control/lightTimer"
    ),
    data
)
    .then(function () {

        lightTimer = data;

        /*
           Nếu bật hẹn giờ,
           hủy lệnh điều khiển tay đang chờ của đèn.
        */
        if (lightTimer.enabled) {
            cancelPendingCommand("light");
        }

        updateLightTimerUI();
        updateControlModeUI();

    })
    .catch(function (error) {

        setText(
            "lightTimerStatus",
            "❌ Lỗi lưu Firebase: " +
            error.message
        );
    });
 

};

/* =====================================================
LẮNG NGHE HẸN GIỜ ĐÈN
===================================================== */

function listenLightTimer() {


onValue(
    ref(
        database,
        "smartfarm/control/lightTimer"
    ),

    function (snapshot) {

        const data =
            snapshot.val();


        if (!data) {

            lightTimer = {
                enabled: false,
                onTime: "",
                offTime: ""
            };

            updateLightTimerUI();
            updateControlModeUI();

            return;
        }


        lightTimer = {

            enabled:
                Boolean(data.enabled),

            onTime:
                data.onTime || "",

            offTime:
                data.offTime || ""
        };


        /*
           Khi đèn chuyển sang hẹn giờ,
           không tiếp tục chờ lệnh điều khiển tay cũ.
        */
        if (lightTimer.enabled) {
            cancelPendingCommand("light");
        }


        const enabledEl =
            document.getElementById(
                "lightTimerSwitch"
            );

        const onTimeEl =
            document.getElementById(
                "lightOnTime"
            );

        const offTimeEl =
            document.getElementById(
                "lightOffTime"
            );

        const lightSwitch =
            document.getElementById(
                "lightSwitch"
            );


        if (enabledEl) {
            enabledEl.checked =
                lightTimer.enabled;
        }


        if (onTimeEl) {
            onTimeEl.value =
                lightTimer.onTime;
        }


        if (offTimeEl) {
            offTimeEl.value =
                lightTimer.offTime;
        }


        if (lightSwitch) {
            lightSwitch.disabled =
                lightTimer.enabled;
        }


        updateLightTimerUI();
        updateControlModeUI();
    }
);


}

/* =====================================================
HIỂN THỊ TRẠNG THÁI HẸN GIỜ
===================================================== */

function updateLightTimerUI() {

if (!lightTimer.enabled) {

    setText(
        "lightTimerStatus",
        "Chưa bật hẹn giờ"
    );

    return;
}


if (
    !lightTimer.onTime ||
    !lightTimer.offTime
) {

    setText(
        "lightTimerStatus",
        "⚠ Vui lòng chọn giờ bật và giờ tắt"
    );

    return;
}


setText(
    "lightTimerStatus",
    "Đang hẹn giờ: bật " +
    lightTimer.onTime +
    " • tắt " +
    lightTimer.offTime
);


}

/* =====================================================
LƯU CÀI ĐẶT NẮNG / MƯA
===================================================== */

window.saveSmartSettings = async function () {

smartSettings.sunny.temp =
    getRangeNumber("sunTemp");

smartSettings.sunny.humidity =
    getRangeNumber("sunHumidity");

smartSettings.sunny.soil =
    getRangeNumber("sunSoil");


smartSettings.rain.temp =
    getRangeNumber("rainTemp");

smartSettings.rain.humidity =
    getRangeNumber("rainHumidity");

smartSettings.rain.soil =
    getRangeNumber("rainSoil");


try {

    const snapshot =
        await get(
            ref(
                database,
                "smartfarm/settings/version"
            )
        );


    const currentVersion =
        toNumber(
            snapshot.val(),
            0
        );


    settingsVersion =
        currentVersion + 1;


    const data = {

        version:
            settingsVersion,

        sunny:
            smartSettings.sunny,

        rain:
            smartSettings.rain
    };


    await set(
        ref(
            database,
            "smartfarm/settings"
        ),
        data
    );


    smartSettings.version =
        settingsVersion;


    setText(
        "settingStatus",
        "✅ Đã lưu cài đặt. Version: " +
        settingsVersion
    );

} catch (error) {

    setText(
        "settingStatus",
        "❌ Lỗi lưu cài đặt: " +
        error.message
    );
}


};

/* =====================================================
LẮNG NGHE CÀI ĐẶT NẮNG / MƯA
===================================================== */

function listenSmartSettings() {


onValue(
    ref(
        database,
        "smartfarm/settings"
    ),

    function (snapshot) {

        const data =
            snapshot.val();


        if (!data) {
            return;
        }


        settingsVersion =
            toNumber(
                data.version,
                settingsVersion
            );


        smartSettings.version =
            settingsVersion;


        /*
           Cài đặt trời nắng.
        */
        if (data.sunny) {

            smartSettings.sunny = {

                temp:
                    toNumber(
                        data.sunny.temp,
                        32
                    ),

                humidity:
                    toNumber(
                        data.sunny.humidity,
                        60
                    ),

                soil:
                    toNumber(
                        data.sunny.soil,
                        20
                    )
            };


            setRangeValue(
                "sunTemp",
                smartSettings.sunny.temp
            );

            setRangeValue(
                "sunHumidity",
                smartSettings.sunny.humidity
            );

            setRangeValue(
                "sunSoil",
                smartSettings.sunny.soil
            );
        }


        /*
           Cài đặt trời mưa.
        */
        if (data.rain) {

            smartSettings.rain = {

                temp:
                    toNumber(
                        data.rain.temp,
                        30
                    ),

                humidity:
                    toNumber(
                        data.rain.humidity,
                        70
                    ),

                soil:
                    toNumber(
                        data.rain.soil,
                        70
                    )
            };


            setRangeValue(
                "rainTemp",
                smartSettings.rain.temp
            );

            setRangeValue(
                "rainHumidity",
                smartSettings.rain.humidity
            );

            setRangeValue(
                "rainSoil",
                smartSettings.rain.soil
            );
        }
    }
);


}

/* =====================================================
CẬP NHẬT GIÁ TRỊ RANGE
===================================================== */

window.updateRangeValue = function (id) {


const input =
    document.getElementById(id);

const output =
    document.getElementById(
        id + "Value"
    );


if (input && output) {
    output.innerText =
        input.value;
}


};

/* =====================================================
SET RANGE TỪ FIREBASE
===================================================== */

function setRangeValue(id, value) {


const input =
    document.getElementById(id);

const output =
    document.getElementById(
        id + "Value"
    );


if (input) {
    input.value = value;
}


if (output) {
    output.innerText = value;
}


}

/* =====================================================
LẤY GIÁ TRỊ RANGE
===================================================== */

function getRangeNumber(id) {


const input =
    document.getElementById(id);


if (!input) {
    return 0;
}


return Number(input.value);


}

/* =====================================================
CHỮ TRẠNG THÁI THIẾT BỊ
===================================================== */

function getStateText(device, value)
{
    if(device === "servo")
    {
        return Number(value) === 0
            ? "MỞ"
            : "ĐÓNG";
    }

    return Number(value) === 1
        ? "ON"
        : "OFF";
}

/* =====================================================
TÊN THIẾT BỊ
===================================================== */

function getDeviceName(device) {


if (device === "pump") {
    return "Máy bơm";
}


if (device === "light") {
    return "Đèn";
}


if (device === "servo") {
    return "Mái che";
}


return device;


}

/* =====================================================
CHUYỂN GIÁ TRỊ SANG NUMBER
===================================================== */

function toNumber(value, fallback = 0) {


const number =
    Number(value);


if (Number.isNaN(number)) {
    return fallback;
}


return number;


}

/* =====================================================
SET TEXT AN TOÀN
===================================================== */

function setText(id, text) {


const element =
    document.getElementById(id);


if (element) {
    element.innerText = text;
}


}
