from __future__ import annotations

from typing import NamedTuple, TYPE_CHECKING

from BaseClasses import MultiWorld, Region

from .Enums import Regions, Locations
from .Locations import MM2ShipLocation, location_table

if TYPE_CHECKING:
    from . import MM2ShipWorld


class MM2ShipRegionData(NamedTuple):
    connecting_regions: list[str] = []


class MM2ShipRegion(Region):
    game = "2 Ship 2 Harkinian (MM)"

    def __init__(self, name: str, player: int, multiworld: MultiWorld, hint: str | None = None):
        super().__init__(name, player, multiworld, hint)


def create_regions_and_locations(world: "MM2ShipWorld") -> None:
    region_data_table: dict[str, MM2ShipRegionData] = {}

    # Regions enum -> region name strings
    for entry in Regions:
        region_data_table[entry.value] = MM2ShipRegionData([])

    # Start region (not part of enum)
    region_data_table.setdefault("Menu", MM2ShipRegionData(["Clock Town South"]))

    # Safety: ensure the hub exists
    region_data_table.setdefault("Clock Town South", MM2ShipRegionData([]))

    # Create regions
    for region_name, data in region_data_table.items():
        region = MM2ShipRegion(region_name, world.player, world.multiworld)
        world.multiworld.regions.append(region)
        region.add_exits(data.connecting_regions)

    hub = world.get_region("Clock Town South")

    # Define locations to exclude based on shuffle options
    frog_locations = {
       # Locations.MOUNTAIN_VILLAGE_FROG_CHOIR,
        Locations.CLOCK_TOWN_LAUNDRY_FROG,
        Locations.GREAT_BAY_TEMPLE_GEKKO_FROG,
        Locations.SOUTHERN_SWAMP_FROG,
        Locations.WOODFALL_TEMPLE_GEKKO_FROG,
    }

    # Define Termina Field grass locations (excludes grotto grass)
    termina_field_grass_locations = {
        Locations[f"TERMINA_FIELD_GRASS_{i:02d}"]
        for i in range(1, 217)
    }

    # Define cow grotto grass locations
    # 72 Termina Field Cow Grotto grass + 72 Great Bay Cow Grotto grass = 144 total
    cow_grotto_grass_locations = {
        Locations[f"TERMINA_FIELD_COW_GROTTO_GRASS_{i:02d}"]
        for i in range(1, 73)
    } | {
        Locations[f"GREAT_BAY_COAST_COW_GROTTO_GRASS_{i:02d}"]
        for i in range(1, 73)
    }

    # Create locations and attach to hub, filtering based on options
    from .Locations import location_data_table
    for loc in Locations:
        # Skip frog locations if shuffle_frogs is OFF
        # (Frogs behave as vanilla when not shuffled)
        if not world.options.shuffle_frogs.value and loc in frog_locations:
            continue

        # Skip Termina Field grass if option is ON and grass is shuffled
        if (world.options.exclude_termina_field_grass.value and
            world.options.shuffle_grass_drops.value and
            loc in termina_field_grass_locations):
            continue

        # Skip cow grotto grass if option is ON and grass is shuffled
        if (world.options.exclude_cow_grotto_grass.value and
            world.options.shuffle_grass_drops.value and
            loc in cow_grotto_grass_locations):
            continue

        # NOTE: Map/compass LOCATIONS are always created, even when starting_maps_and_compasses is ON
        # When starting with maps/compasses, the items are removed from the pool, not the locations

        loc_name = loc.value
        address = location_data_table[loc]
        # Event locations (address=None) are created but not given a network ID
        loc_obj = MM2ShipLocation(world.player, loc_name, address, hub)
        hub.locations.append(loc_obj)

        # Place Victory event item at Victory location
        if loc == Locations.VICTORY:
            loc_obj.place_locked_item(world.create_item("Victory", create_as_event=True))

    if not hasattr(world, "included_locations") or world.included_locations is None:
        world.included_locations = {}
