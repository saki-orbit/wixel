CREATE TABLE `booth_state` (
	`device_id` text PRIMARY KEY NOT NULL,
	`occupied` integer NOT NULL,
	`target_type` text NOT NULL,
	`distance_cm` integer,
	`uptime_ms` integer NOT NULL,
	`updated_at` text NOT NULL,
	`occupied_since` text
);
