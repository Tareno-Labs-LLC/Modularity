using System.Collections;
using System.Collections.Generic;
using UnityEngine;

public class autoEnableAndDisableListsOfObjectsAfterAmountOfTime : MonoBehaviour
{
    public List<GameObject> objectsToEnable;
    public List<GameObject> objectsToDisable;
    public float timeToEnableObjects;
    private float timer;

    private void Start()
    {
        timer = 0;
    }

    private void Update()
    {
        timer += Time.deltaTime;
        if (timer >= timeToEnableObjects)
        {
            EnableObjects();
            DisableObjects();
            timer = 0;
        }
    }

    private void EnableObjects()
    {
        foreach (var obj in objectsToEnable)
        {
            if (obj != null)
            {
                obj.SetActive(true);

                // Check for Collider (3D)
                Collider collider = obj.GetComponent<Collider>();
                if (collider != null && !collider.enabled)
                {
                    collider.enabled = true; // Enable the Collider if it's disabled
                }

                // Check for Collider2D (2D)
                Collider2D collider2D = obj.GetComponent<Collider2D>();
                if (collider2D != null && !collider2D.enabled)
                {
                    collider2D.enabled = true; // Enable the Collider2D if it's disabled
                }
            }
        }
    }

    private void DisableObjects()
    {
        foreach (var obj in objectsToDisable)
        {
            if (obj != null)
            {
                obj.SetActive(false);
            }
        }
    }
}
