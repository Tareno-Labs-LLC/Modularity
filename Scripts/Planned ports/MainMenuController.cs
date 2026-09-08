using UnityEngine;
using System.Collections;
using TMPro;
using UnityEngine.UI;

public class MainMenuController : MonoBehaviour
{
    public enum MenuOrientation { Vertical, Horizontal }
    public MenuOrientation menuOrientation = MenuOrientation.Vertical;

    public RectTransform heartImage;
    public TextMeshProUGUI[] menuItems;
    public AudioSource MenuMove;
    public AudioSource MenuSelect;
    public float moveSpeed = 0.3f;
    public float offset = 20f;

    public MenuAction[] menuActions;

    private int currentIndex = 0;
    private float inputDelay = 0.2f;
    private float nextInputTime = 0f;
    private float initialDelay = 0.2f;
    private bool canMove = false;

    [System.Serializable]
    public class MenuAction
    {
        public GameObject[] enableObjects;
        public GameObject[] disableObjects;
    }

    void Start()
    {
        StartCoroutine(InitialDelayCoroutine());
    }

    IEnumerator InitialDelayCoroutine()
    {
        yield return new WaitForSeconds(initialDelay);
        canMove = true;
    }

    void Update()
    {
        if (canMove && Time.time >= nextInputTime)
        {
            if (menuOrientation == MenuOrientation.Vertical)
            {
                if (Input.GetKeyDown(KeyCode.DownArrow) || Input.GetKeyDown(KeyCode.S) || (Input.GetAxisRaw("Vertical") < -0.5f && Mathf.Approximately(Input.GetAxisRaw("Vertical"), -1)))
                {
                    Navigate(1);
                    MenuMove.Play();
                    nextInputTime = Time.time + inputDelay;
                }
                else if (Input.GetKeyDown(KeyCode.UpArrow) || Input.GetKeyDown(KeyCode.W) || (Input.GetAxisRaw("Vertical") > 0.5f && Mathf.Approximately(Input.GetAxisRaw("Vertical"), 1)))
                {
                    Navigate(-1);
                    MenuMove.Play();
                    nextInputTime = Time.time + inputDelay;
                }
            }
            else if (menuOrientation == MenuOrientation.Horizontal)
            {
                if (Input.GetKeyDown(KeyCode.RightArrow) || Input.GetKeyDown(KeyCode.D) || (Input.GetAxisRaw("Horizontal") > 0.5f && Mathf.Approximately(Input.GetAxisRaw("Horizontal"), 1)))
                {
                    Navigate(1);
                    MenuMove.Play();
                    nextInputTime = Time.time + inputDelay;
                }
                else if (Input.GetKeyDown(KeyCode.LeftArrow) || Input.GetKeyDown(KeyCode.A) || (Input.GetAxisRaw("Horizontal") < -0.5f && Mathf.Approximately(Input.GetAxisRaw("Horizontal"), -1)))
                {
                    Navigate(-1);
                    MenuMove.Play();
                    nextInputTime = Time.time + inputDelay;
                }
            }

            // Check for keyboard and controller input for selection
            if (Input.GetKeyDown(KeyCode.Return) || Input.GetKeyDown(KeyCode.KeypadEnter) || Input.GetButtonDown("Submit"))
            {
                ExecuteAction();
                MenuSelect.Play();
            }
        }
    }

    void Navigate(int direction)
    {
        currentIndex += direction;

        if (currentIndex < 0)
        {
            currentIndex = menuItems.Length - 1;
        }
        else if (currentIndex >= menuItems.Length)
        {
            currentIndex = 0;
        }

        MoveHeartToCurrentItem();
    }

    void MoveHeartToCurrentItem()
    {
        if (menuItems.Length > 0)
        {
            RectTransform targetRect = menuItems[currentIndex].GetComponent<RectTransform>();
            Vector2 targetPosition = targetRect.anchoredPosition;

            if (menuOrientation == MenuOrientation.Vertical)
            {
                targetPosition.x -= targetRect.rect.width / 2 + heartImage.rect.width / 2 + offset;
            }
            else
            {
                targetPosition.y -= targetRect.rect.height / 2 + heartImage.rect.height / 2 + offset;
            }

            StartCoroutine(MoveHeart(targetPosition));
        }
    }

    System.Collections.IEnumerator MoveHeart(Vector2 targetPosition)
    {
        Vector2 startPos = heartImage.anchoredPosition;
        float elapsedTime = 0f;

        while (elapsedTime < 1f)
        {
            heartImage.anchoredPosition = Vector2.Lerp(startPos, targetPosition, elapsedTime);
            elapsedTime += Time.deltaTime / moveSpeed;
            yield return null;
        }

        heartImage.anchoredPosition = targetPosition;
    }

    void ExecuteAction()
    {
        if (currentIndex < menuActions.Length)
        {
            var action = menuActions[currentIndex];

            foreach (GameObject obj in action.disableObjects)
            {
                obj.SetActive(false);
            }

            foreach (GameObject obj in action.enableObjects)
            {
                obj.SetActive(true);
            }
        }
        else
        {
            Debug.LogWarning("No action assigned for this menu item.");
        }
    }
}
