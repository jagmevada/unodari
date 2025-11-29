// ---------------- Supabase Setup ----------------
const SUPABASE_URL = "https://akxcjabakrvfaevdfwru.supabase.co";
const SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImFreGNqYWJha3J2ZmFldmRmd3J1Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NDkxMjMwMjUsImV4cCI6MjA2NDY5OTAyNX0.kykki4uVVgkSVU4lH-wcuGRdyu2xJ1CQkYFhQq_u08w";

const supabase = window.supabase.createClient(SUPABASE_URL, SUPABASE_ANON_KEY);

// List of devices
const devices = ["uno_1", "uno_2", "uno_3"];

// Meal time ranges (in 24-hour format)
const mealTimes = {
    breakfast: { start: 6, end: 9 },    // 6 AM - 9 AM
    lunch: { start: 11, end: 15 },       // 11 AM - 3 PM
    dinner: { start: 17, end: 22 }       // 5 PM - 10 PM
};

// Function to determine current meal time
function getCurrentMealPeriod() {
    const hour = new Date().getHours()+12;
    
    if (hour >= mealTimes.breakfast.start && hour < mealTimes.breakfast.end) {
        return 'breakfast';
    } else if (hour >= mealTimes.lunch.start && hour < mealTimes.lunch.end) {
        return 'lunch';
    } else if (hour >= mealTimes.dinner.start && hour < mealTimes.dinner.end) {
        return 'dinner';
    }
    return null;
}

// Function to highlight active meal rows
function highlightActiveMeal() {
    const activeMeal = getCurrentMealPeriod();
    
    // Remove all active classes
    document.querySelectorAll('.row.active').forEach(row => row.classList.remove('active'));
    
    if (activeMeal) {
        // Highlight all rows of the active meal type
        document.querySelectorAll(`.row .${activeMeal}`).forEach(element => {
            element.closest('.row').classList.add('active');
        });
    }
}

// Main function to load all data
async function loadData() {
    const mealData = {
        breakfast: { devices: {}, total: 0, timestamp: '' },
        lunch: { devices: {}, total: 0, timestamp: '' },
        dinner: { devices: {}, total: 0, timestamp: '' }
    };
    reorderMealCards();
    let grandTotal = 0;

    // Get today's date with timezone consideration
    const now = new Date();
    const year = now.getFullYear();
    const month = String(now.getMonth() + 1).padStart(2, '0');
    const day = String(now.getDate()).padStart(2, '0');
    const today = `${year}-${month}-${day}`;

    // Fetch data for each device
    for (let sensor of devices) {
        

        const { data, error } = await supabase
            .from("unodari_token")
            .select("*")
            .eq("sensor_id", sensor)
            .eq("date", today)
            .single();

        if (error || !data) continue;

        const { breakfast, lunch, dinner, total, timestamp } = data;

        // Organize by meal type
        mealData.breakfast.devices[sensor] = breakfast;
        mealData.breakfast.total += breakfast;
        mealData.breakfast.timestamp = timestamp;

        mealData.lunch.devices[sensor] = lunch;
        mealData.lunch.total += lunch;
        mealData.lunch.timestamp = timestamp;

        mealData.dinner.devices[sensor] = dinner;
        mealData.dinner.total += dinner;
        mealData.dinner.timestamp = timestamp;

        grandTotal += total;
    }

    // Update meal cards with device counts
    for (const meal of ['breakfast', 'lunch', 'dinner']) {
        const mealInfo = mealData[meal];
        
        // Update individual device counts
        for (const device of devices) {
            const count = mealInfo.devices[device] || 0;
            const element = document.querySelector(`b.device-count[data-device="${device}"][data-meal="${meal}"]`);
            if (element) element.innerText = count;
        }

        // Update meal total
        const totalElement = document.querySelector(`b.meal-total[data-meal="${meal}"]`);
        if (totalElement) totalElement.innerText = mealInfo.total;

        // Update timestamp
        const timestampElement = document.querySelector(`p.timestamp[data-meal="${meal}"]`);
        if (timestampElement) {
            timestampElement.innerText = `Last Updated: ${mealInfo.timestamp ? formatTimestamp(mealInfo.timestamp) : '-'}`;
        }
    }

    // Update totals section
    document.getElementById("total-breakfast").innerText = mealData.breakfast.total;
    document.getElementById("total-lunch").innerText = mealData.lunch.total;
    document.getElementById("total-dinner").innerText = mealData.dinner.total;
    document.getElementById("grand-total").innerText = grandTotal;
    
    // Reorder cards with active meal on top (after data is loaded)
    reorderMealCards();
    
    // Highlight active meal period
    highlightActiveMeal();
}

// Function to reorder meal cards with active meal on top
function reorderMealCards() {
    const container = document.getElementById('container');
    const activeMeal = getCurrentMealPeriod();
    
    const meals = ['breakfast', 'lunch', 'dinner'];
    
    // Mark inactive cards with inactive class
    meals.forEach(meal => {
        const card = document.querySelector(`[data-meal="${meal}"]`);
        if (card) {
            if (activeMeal && meal !== activeMeal) {
                card.classList.add('inactive');
            } else {
                card.classList.remove('inactive');
            }
        }
    });
    
    if (!activeMeal) return;

    const mealOrder = [activeMeal, ...meals.filter(m => m !== activeMeal)];

    mealOrder.forEach(meal => {
        const card = document.querySelector(`[data-meal="${meal}"]`);
        if (card) {
            container.appendChild(card); // Move to end (which reorders them)
        }
    });
}

// Format timestamp nicely
function formatTimestamp(ts) {
    const date = new Date(ts);
    return date.toLocaleString();
}

// First load
loadData();

// Auto-refresh every 10 seconds
setInterval(loadData, 10000);

// Update highlight every minute to catch time changes
setInterval(highlightActiveMeal, 60000);
